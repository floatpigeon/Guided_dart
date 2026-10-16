#!/usr/bin/env python3
"""Convert the board's IMU log v1 into a host-only ROS 2 bag."""

import argparse
import csv
from dataclasses import dataclass
import math
from pathlib import Path
import sys
import tempfile


INTEGER_FIELDS = (
    'sequence', 'recorded_ns', 'timestamp_ns', 'generation', 'quality_events',
    'sample_accepted', 'integrated', 'has_reference_frame', 'command_accepted',
    'command_quality_events', 'read_error', 'dropped',
    'raw_ax', 'raw_ay', 'raw_az', 'raw_gx', 'raw_gy', 'raw_gz', 'sample_flags',
)
FLOAT_FIELDS = (
    'ax_m_s2', 'ay_m_s2', 'az_m_s2', 'gx_rad_s', 'gy_rad_s', 'gz_rad_s',
    'temperature_c', 'qw', 'qx', 'qy', 'qz', 'roll_rad', 'pitch_rad', 'yaw_rad',
    'wx_rad_s', 'wy_rad_s', 'wz_rad_s',
    'linear_ax_m_s2', 'linear_ay_m_s2', 'linear_az_m_s2',
    'vx_m_s', 'vy_m_s', 'vz_m_s', 'rotation_x_rad', 'rotation_y_rad', 'rotation_z_rad',
    'seed_qw', 'seed_qx', 'seed_qy', 'seed_qz',
)
BOOLEAN_FIELDS = (
    'sample_accepted', 'integrated', 'has_reference_frame', 'command_accepted',
)
SECOND_NS = 1_000_000_000


@dataclass(frozen=True)
class Record:
    session: int
    source: str
    line: int
    metadata: dict
    text: dict
    values: dict

    def vector(self, names):
        return tuple(self.values[name] for name in names)


def read_records(paths, skip_bad_rows=False):
    """Stream appended sessions; fail with a source line instead of guessing fields."""
    session = 0
    for path in paths:
        header = None
        metadata = {}
        waiting_header = False
        last_sequence = 0
        last_recorded = 0
        with Path(path).open(encoding='utf-8-sig', newline='') as source:
            for line_number, line in enumerate(source, 1):
                line = line.strip()
                if not line:
                    continue
                location = f'{path}:{line_number}'
                if line.startswith('# imu_log '):
                    metadata = dict(
                        item.split('=', 1) for item in line.split()[2:] if '=' in item
                    )
                    if metadata.get('version') != '1':
                        raise ValueError(f'{location}: unsupported IMU log version')
                    session += 1
                    header = None
                    waiting_header = True
                    last_sequence = last_recorded = 0
                    continue
                if line.startswith('#'):
                    continue
                try:
                    fields = next(csv.reader([line], strict=True))
                except csv.Error as error:
                    if not skip_bad_rows:
                        raise ValueError(f'{location}: {error}') from error
                    print(f'warning: skipping {location}: {error}', file=sys.stderr)
                    continue
                if fields[0] == 'event':
                    required = {'event', *INTEGER_FIELDS, *FLOAT_FIELDS}
                    missing = required.difference(fields)
                    if missing or len(set(fields)) != len(fields):
                        raise ValueError(f'{location}: invalid header; missing {sorted(missing)}')
                    if not waiting_header:
                        session += 1
                        metadata = {}
                        last_sequence = last_recorded = 0
                    header = fields
                    waiting_header = False
                    continue
                try:
                    if header is None or len(fields) != len(header):
                        raise ValueError('missing header or incomplete CSV row')
                    text = dict(zip(header, fields))
                    if text['event'] not in ('sample', 'seed', 'reset', 'read_error'):
                        raise ValueError(f'unknown event {text["event"]!r}')
                    values = {name: int(text[name]) for name in INTEGER_FIELDS}
                    values.update({name: float(text[name]) for name in FLOAT_FIELDS})
                    if any(values[name] not in (0, 1) for name in BOOLEAN_FIELDS):
                        raise ValueError('boolean fields must be 0 or 1')
                    if values['sequence'] <= last_sequence or values['recorded_ns'] < last_recorded:
                        raise ValueError('event sequence or processing time moved backwards')
                    if values['recorded_ns'] <= 0 or any(
                        values[name] < 0 for name in (
                            'timestamp_ns', 'generation', 'quality_events',
                            'command_quality_events', 'dropped', 'sample_flags',
                        )
                    ):
                        raise ValueError('negative counter or invalid timestamp')
                    if any(values[name] >= 2**64 for name in (
                        'sequence', 'recorded_ns', 'timestamp_ns', 'generation', 'dropped',
                    )):
                        raise ValueError('counter exceeds uint64 range')
                    if any(not -32768 <= values[name] <= 32767 for name in (
                        'raw_ax', 'raw_ay', 'raw_az', 'raw_gx', 'raw_gy', 'raw_gz',
                    )):
                        raise ValueError('raw count exceeds int16 range')
                    last_sequence = values['sequence']
                    last_recorded = values['recorded_ns']
                    yield Record(session, str(path), line_number, metadata, text, values)
                except ValueError as error:
                    if not skip_bad_rows:
                        raise ValueError(f'{location}: {error}') from error
                    print(f'warning: skipping {location}: {error}', file=sys.stderr)
        if waiting_header:
            raise ValueError(f'{path}: session metadata has no CSV header')


class Timeline:
    """Preserve processing order; each recorded session gets a separate time origin."""

    def __init__(self):
        self.session = None
        self.origin = 0
        self.base = 0
        self.last = 0

    def map(self, record):
        if record.session != self.session:
            self.session = record.session
            self.origin = record.values['recorded_ns']
            self.base = self.last + SECOND_NS
        stamp = max(self.base + record.values['recorded_ns'] - self.origin, self.last + 1)
        if stamp // SECOND_NS > 2**31 - 1:
            raise ValueError('recording exceeds the ROS Time seconds range')
        self.last = stamp
        return stamp


class Motion:
    """Use logged attitudes; optionally integrate logged reference-frame velocity."""

    def __init__(self, position_mode):
        self.position_mode = position_mode
        self.key = None
        self.position = (0.0, 0.0, 0.0)
        self.previous = None
        self.sequence = None
        self.dropped = 0

    def update(self, record):
        value = record.values
        key = (record.session, value['generation'])
        if key != self.key:
            self.key = key
            self.position = (0.0, 0.0, 0.0)
            self.previous = None
            self.sequence = None
            self.dropped = 0
        continuous = (
            self.sequence is not None and value['sequence'] == self.sequence + 1
            and value['dropped'] == self.dropped
        )
        self.sequence = value['sequence']
        self.dropped = value['dropped']
        event = record.text['event']
        if event == 'seed' and not value['command_accepted']:
            if not continuous:
                self.previous = None
            return None
        if event not in ('sample', 'seed') or not (
            value['sample_accepted'] and value['has_reference_frame']
            and (event != 'seed' or value['command_accepted'])
        ):
            self.previous = None
            return None
        quaternion = record.vector(('qx', 'qy', 'qz', 'qw'))
        velocity = record.vector(('vx_m_s', 'vy_m_s', 'vz_m_s'))
        norm = math.sqrt(sum(component * component for component in quaternion))
        if not math.isfinite(norm) or norm < 1e-12 or not all(map(math.isfinite, velocity)):
            self.previous = None
            return None
        timestamp = value['timestamp_ns']
        if event == 'seed':
            self.position = (0.0, 0.0, 0.0)
            self.previous = None
        if self.position_mode == 'velocity' and self.previous is not None and continuous:
            previous_time, previous_velocity = self.previous
            interval = timestamp - previous_time
            maximum = int(record.metadata.get('max_interval_ns', 100_000_000))
            if value['integrated'] and 0 < interval <= maximum:
                dt = interval / SECOND_NS
                self.position = tuple(
                    position + (before + after) * 0.5 * dt
                    for position, before, after in zip(self.position, previous_velocity, velocity)
                )
        self.previous = (timestamp, velocity)
        return self.position, tuple(component / norm for component in quaternion)


class BagWriter:
    """ROS imports stay at the host boundary; parsing and geometry need only stdlib."""

    def __init__(self, directory, storage):
        try:
            import rosbag2_py
            from rclpy.serialization import serialize_message
            from builtin_interfaces.msg import Time
            from diagnostic_msgs.msg import DiagnosticArray, DiagnosticStatus, KeyValue
            from geometry_msgs.msg import PoseStamped, TransformStamped, Vector3Stamped
            from sensor_msgs.msg import Imu, Temperature
            from std_msgs.msg import Header, UInt64
            from tf2_msgs.msg import TFMessage
        except ImportError as error:
            raise RuntimeError(
                'ROS 2 Python modules unavailable; source a ROS 2 setup file and use '
                '/usr/bin/python3 (see tools/README.md)'
            ) from error
        self.types = {
            cls.__name__: cls for cls in (
                Time, DiagnosticArray, DiagnosticStatus, KeyValue, PoseStamped,
                TransformStamped, Vector3Stamped, Imu, Temperature, Header, UInt64, TFMessage,
            )
        }
        self.serialize = serialize_message
        self.writer = rosbag2_py.SequentialWriter()
        self.writer.open(
            rosbag2_py.StorageOptions(uri=str(directory), storage_id=storage),
            rosbag2_py.ConverterOptions('cdr', 'cdr'),
        )
        self.topics = (
            ('/tf', 'tf2_msgs/msg/TFMessage'),
            ('/imu/raw', 'sensor_msgs/msg/Imu'),
            ('/imu/temperature', 'sensor_msgs/msg/Temperature'),
            ('/imu/pose', 'geometry_msgs/msg/PoseStamped'),
            ('/imu/velocity', 'geometry_msgs/msg/Vector3Stamped'),
            ('/imu/rotation', 'geometry_msgs/msg/Vector3Stamped'),
            ('/imu/rpy', 'geometry_msgs/msg/Vector3Stamped'),
            ('/imu/linear_acceleration', 'geometry_msgs/msg/Vector3Stamped'),
            ('/imu/raw_counts/acceleration', 'geometry_msgs/msg/Vector3Stamped'),
            ('/imu/raw_counts/gyroscope', 'geometry_msgs/msg/Vector3Stamped'),
            ('/imu/generation', 'std_msgs/msg/UInt64'),
            ('/imu/status', 'diagnostic_msgs/msg/DiagnosticArray'),
        )
        for index, (name, type_name) in enumerate(self.topics, 1):
            self.writer.create_topic(rosbag2_py.TopicMetadata(
                id=index, name=name, type=type_name, serialization_format='cdr',
            ))

    def make(self, kind, **fields):
        return self.types[kind](**fields)

    def header(self, stamp, frame):
        return self.make('Header', frame_id=frame, stamp=self.make(
            'Time', sec=stamp // SECOND_NS, nanosec=stamp % SECOND_NS,
        ))

    def write(self, topic, message, stamp):
        self.writer.write(topic, self.serialize(message), stamp)

    def vector(self, topic, components, stamp, frame):
        message = self.make('Vector3Stamped', header=self.header(stamp, frame))
        message.vector.x, message.vector.y, message.vector.z = map(float, components)
        self.write(topic, message, stamp)

    def raw(self, record, stamp, child):
        if record.text['event'] != 'sample':
            return
        message = self.make('Imu', header=self.header(stamp, child))
        message.orientation_covariance[0] = -1.0  # No orientation in the raw measurement.
        message.angular_velocity.x, message.angular_velocity.y, message.angular_velocity.z = (
            record.vector(('gx_rad_s', 'gy_rad_s', 'gz_rad_s'))
        )
        acceleration = message.linear_acceleration
        acceleration.x, acceleration.y, acceleration.z = record.vector(
            ('ax_m_s2', 'ay_m_s2', 'az_m_s2'))
        self.write('/imu/raw', message, stamp)
        self.write('/imu/temperature', self.make(
            'Temperature', header=self.header(stamp, child),
            temperature=record.values['temperature_c'],
        ), stamp)
        self.vector('/imu/raw_counts/acceleration',
                    record.vector(('raw_ax', 'raw_ay', 'raw_az')), stamp, child)
        self.vector('/imu/raw_counts/gyroscope',
                    record.vector(('raw_gx', 'raw_gy', 'raw_gz')), stamp, child)

    def pose(self, record, stamp, reference, child, motion):
        position, quaternion = motion
        # 'world' is a display anchor only. Each generation has its own reference.
        anchor = self.make('TransformStamped', header=self.header(stamp, 'world'),
                           child_frame_id=reference)
        anchor.transform.rotation.w = 1.0
        transform = self.make('TransformStamped', header=self.header(stamp, reference),
                              child_frame_id=child)
        translation = transform.transform.translation
        translation.x, translation.y, translation.z = position
        rotation = transform.transform.rotation
        rotation.x, rotation.y, rotation.z, rotation.w = quaternion
        self.write('/tf', self.make('TFMessage', transforms=[anchor, transform]), stamp)
        pose = self.make('PoseStamped', header=self.header(stamp, reference))
        pose.pose.position.x, pose.pose.position.y, pose.pose.position.z = position
        pose.pose.orientation = transform.transform.rotation
        self.write('/imu/pose', pose, stamp)
        for topic, fields in (
            ('/imu/velocity', ('vx_m_s', 'vy_m_s', 'vz_m_s')),
            ('/imu/rotation', ('rotation_x_rad', 'rotation_y_rad', 'rotation_z_rad')),
            ('/imu/rpy', ('roll_rad', 'pitch_rad', 'yaw_rad')),
            ('/imu/linear_acceleration', ('linear_ax_m_s2', 'linear_ay_m_s2', 'linear_az_m_s2')),
        ):
            self.vector(topic, record.vector(fields), stamp, reference)

    def status(self, record, stamp, reference, child, visualized, position_mode):
        value = record.values
        event = record.text['event']
        rejected = (event == 'sample' and not value['sample_accepted']) or (
            event == 'seed' and not value['command_accepted']
        )
        level = 2 if event == 'read_error' else int(bool(
            rejected or value['quality_events'] or value['command_quality_events']
            or value['dropped']
            or (event == 'sample' and not visualized)
        ))
        details = {
            **record.text, 'session': str(record.session), 'source': record.source,
            'source_line': str(record.line), 'reference_frame': reference, 'sensor_frame': child,
            'visualized': str(int(visualized)), 'position_mode': position_mode,
            **{f'metadata.{key}': text for key, text in record.metadata.items()},
        }
        status = self.make(
            'DiagnosticStatus',
            level=getattr(self.types['DiagnosticStatus'], ('OK', 'WARN', 'ERROR')[level]),
            name='dart/imu', message=event,
            hardware_id='recorded_imu', values=[
                self.make('KeyValue', key=key, value=text) for key, text in details.items()
            ],
        )
        self.write('/imu/status', self.make(
            'DiagnosticArray', header=self.header(stamp, reference), status=[status],
        ), stamp)
        self.write('/imu/generation', self.make('UInt64', data=value['generation']), stamp)

    def close(self):
        self.writer.close()


def convert(paths, output, storage='mcap', position='fixed', session=None, generation=None,
            skip_bad_rows=False):
    output = Path(output).absolute()
    if output.exists():
        raise ValueError(f'output already exists: {output}')
    output.parent.mkdir(parents=True, exist_ok=True)
    count = poses = 0
    segments = set()
    timeline = Timeline()
    motion = Motion(position)
    # Failed conversion never leaves a plausible but incomplete output bag.
    with tempfile.TemporaryDirectory(prefix='.imu-bag-', dir=output.parent) as temporary:
        directory = Path(temporary) / 'bag'
        writer = BagWriter(directory, storage)
        try:
            for record in read_records(paths, skip_bad_rows):
                if session is not None and record.session != session:
                    continue
                if generation is not None and record.values['generation'] != generation:
                    continue
                stamp = timeline.map(record)
                suffix = f's{record.session}_g{record.values["generation"]}'
                reference = f'imu_reference_{suffix}'
                child = f'imu_link_{suffix}'
                state = motion.update(record)
                writer.raw(record, stamp, child)
                if state is not None:
                    writer.pose(record, stamp, reference, child, state)
                    poses += 1
                    segments.add((reference, child))
                writer.status(record, stamp, reference, child, state is not None, position)
                count += 1
        finally:
            writer.close()
        if count == 0:
            raise ValueError('no matching IMU records')
        if output.exists():
            raise ValueError(f'output appeared during conversion: {output}')
        directory.rename(output)
    return {'records': count, 'poses': poses, 'segments': sorted(segments)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', type=Path, nargs='+', help='IMU v1 logs, in recording order')
    parser.add_argument('-o', '--output', type=Path, required=True, help='new ROS 2 bag directory')
    parser.add_argument('--storage', choices=('mcap', 'sqlite3'), default='mcap')
    parser.add_argument('--position', choices=('fixed', 'velocity'), default='fixed',
                        help='fixed origin (default), or an offline velocity integral')
    parser.add_argument('--session', type=int,
                        help='select a session, numbered from 1 across input files')
    parser.add_argument('--generation', type=int, help='select a logged integration generation')
    parser.add_argument('--skip-bad-rows', action='store_true',
                        help='skip damaged rows with warnings')
    args = parser.parse_args(argv)
    if args.session is not None and args.session < 1:
        parser.error('--session must be at least 1')
    if args.generation is not None and args.generation < 0:
        parser.error('--generation must be nonnegative')
    try:
        result = convert(args.logs, args.output, args.storage, args.position,
                         args.session, args.generation, args.skip_bad_rows)
    except (OSError, ValueError, RuntimeError, csv.Error) as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    print(f'{args.output}: {result["records"]} records, {result["poses"]} poses')
    print(f'Position mode: {args.position}; Foxglove 3D display frame: world')
    for reference, child in result['segments']:
        print(f'  world -> {reference} -> {child}')
    if not result['poses']:
        print('warning: no valid referenced attitudes; inspect /imu/status', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
