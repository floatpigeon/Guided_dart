#!/usr/bin/env python3
"""Host regression tests; ROS round trips run when ROS 2 has been sourced."""

import csv
import importlib.util
import io
import math
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS_DIR = Path(__file__).resolve().parents[1] / 'tools'
CONVERTER_SPEC = importlib.util.spec_from_file_location(
    'imu_log_to_bag', TOOLS_DIR / 'imu_log_to_bag.py'
)
converter = importlib.util.module_from_spec(CONVERTER_SPEC)
sys.modules[CONVERTER_SPEC.name] = converter
CONVERTER_SPEC.loader.exec_module(converter)


COLUMNS = (
    'event,sequence,recorded_ns,timestamp_ns,generation,quality_events,'
    'sample_accepted,integrated,has_reference_frame,command_accepted,command_quality_events,'
    'read_error,dropped,raw_ax,raw_ay,raw_az,raw_gx,raw_gy,raw_gz,'
    'ax_m_s2,ay_m_s2,az_m_s2,gx_rad_s,gy_rad_s,gz_rad_s,temperature_c,sample_flags,'
    'qw,qx,qy,qz,roll_rad,pitch_rad,yaw_rad,wx_rad_s,wy_rad_s,wz_rad_s,'
    'linear_ax_m_s2,linear_ay_m_s2,linear_az_m_s2,vx_m_s,vy_m_s,vz_m_s,'
    'rotation_x_rad,rotation_y_rad,rotation_z_rad,seed_qw,seed_qx,seed_qy,seed_qz'
).split(',')
HAS_ROS = importlib.util.find_spec('rosbag2_py') is not None


def sample(sequence, **changes):
    row = dict.fromkeys(COLUMNS, 0)
    row.update(
        event='sample', sequence=sequence, recorded_ns=5_000_000_000 + sequence * 10_000_000,
        timestamp_ns=1_000_000_000 + sequence * 10_000_000, generation=1,
        sample_accepted=1, integrated=1, has_reference_frame=1,
        read_error=-1, raw_ax=-123, raw_gy=456, az_m_s2=9.80665,
        qw=1, seed_qw=1, temperature_c=25,
    )
    row.update(changes)
    return row


def log_text(rows, version=1):
    output = io.StringIO()
    output.write(f'# imu_log version={version} max_interval_ns=100000000\n')
    writer = csv.DictWriter(output, fieldnames=COLUMNS)
    writer.writeheader()
    writer.writerows(rows)
    output.write('# session_end\n')
    return output.getvalue()


class LogConversionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.log = self.root / 'imu.log'

    def records(self, rows):
        self.log.write_text(log_text(rows))
        return list(converter.read_records([self.log]))

    def test_session_timeline_and_duplicate_processing_time(self):
        self.log.write_text(log_text([
            sample(1), sample(2, recorded_ns=5_010_000_000), sample(3),
        ]) + log_text([sample(1, recorded_ns=100), sample(2, recorded_ns=200)]))
        records = list(converter.read_records([self.log]))
        self.assertEqual([r.session for r in records], [1, 1, 1, 2, 2])
        timeline = converter.Timeline()
        stamps = [timeline.map(record) for record in records]
        self.assertEqual(stamps, [1_000_000_000, 1_000_000_001, 1_020_000_000,
                                 2_020_000_000, 2_020_000_100])
        other = self.root / 'second.log'
        other.write_text(log_text([sample(1)]))
        self.assertEqual(list(converter.read_records([self.log, other]))[-1].session, 3)

    def test_bad_rows_and_versions(self):
        self.log.write_text(log_text([sample(1)]) + 'sample,2,broken\n')
        with self.assertRaisesRegex(ValueError, r'imu.log:5:'):
            list(converter.read_records([self.log]))
        with patch('sys.stderr', new_callable=io.StringIO) as errors:
            records = list(converter.read_records([self.log], skip_bad_rows=True))
            self.assertEqual(len(records), 1)
            self.assertIn('skipping', errors.getvalue())
        self.log.write_text(log_text([sample(1)], version=2))
        with self.assertRaisesRegex(ValueError, 'unsupported'):
            list(converter.read_records([self.log]))
        with self.assertRaisesRegex(ValueError, 'moved backwards'):
            self.records([sample(2), sample(1)])
        with self.assertRaisesRegex(ValueError, 'boolean fields'):
            self.records([sample(1, integrated=3)])

    def test_geometry_uses_logged_quaternion_and_reseed_boundaries(self):
        records = self.records([
            sample(1, event='seed', command_accepted=1, integrated=0),
            sample(2, qw=math.sqrt(0.5), qz=math.sqrt(0.5), vx_m_s=2),
            sample(3, event='seed', command_accepted=0, command_quality_events=512),
            sample(4, vx_m_s=2),
            sample(5, event='seed', generation=2, command_accepted=1, integrated=0),
            sample(6, generation=2, vx_m_s=2),
            sample(7, generation=3, event='reset', has_reference_frame=0),
        ])
        motion = converter.Motion('velocity')
        result = [motion.update(record) for record in records]
        self.assertEqual(result[0][0], (0, 0, 0))
        self.assertAlmostEqual(result[1][0][0], 0.01)
        self.assertEqual(result[1][1], (0, 0, math.sqrt(0.5), math.sqrt(0.5)))
        self.assertIsNone(result[2])
        self.assertAlmostEqual(result[3][0][0], 0.05)
        self.assertEqual(result[4][0], (0, 0, 0))
        self.assertAlmostEqual(result[5][0][0], 0.01)
        self.assertIsNone(result[6])
        fixed = converter.Motion('fixed')
        self.assertTrue(all(fixed.update(r)[0] == (0, 0, 0) for r in records[:2]))

    def test_invalid_samples_and_missing_intervals_do_not_move_pose(self):
        records = self.records([
            sample(1, vx_m_s=2),
            sample(2, sample_accepted=0, integrated=0, ax_m_s2=float('nan')),
            sample(3, vx_m_s=2),
            sample(4, vx_m_s=2, integrated=0, quality_events=128),
            sample(6, vx_m_s=2, dropped=1),
            sample(7, vx_m_s=2, dropped=1),
            sample(8, event='read_error', read_error=4),
            sample(9, vx_m_s=2),
            sample(10, qw=0),
        ])
        motion = converter.Motion('velocity')
        result = [motion.update(record) for record in records]
        self.assertIsNone(result[1])
        self.assertTrue(math.isnan(records[1].values['ax_m_s2']))
        for index in (0, 2, 3, 4):
            self.assertEqual(result[index][0], (0, 0, 0))
        self.assertAlmostEqual(result[5][0][0], 0.02)
        self.assertIsNone(result[6])
        self.assertAlmostEqual(result[7][0][0], 0.02)
        self.assertIsNone(result[8])

    def test_launch_xml_parses(self):
        try:
            from launch import LaunchContext
            from launch.frontend import Parser
            from launch.utilities import perform_substitutions
        except ImportError:
            self.skipTest('source ROS 2 to check the launch frontend')
        root, parser = Parser.load(TOOLS_DIR / 'foxglove_launcher.xml')
        description = parser.parse_description(root)
        context = LaunchContext()
        context.launch_configurations.update(bag='/tmp/bag with spaces', rate='0.5')
        player = description.entities[-1]
        command = [perform_substitutions(context, argument) for argument in player.cmd]
        self.assertEqual(command, [
            'ros2', 'bag', 'play', '/tmp/bag with spaces', '--clock', '100',
            '--rate', '0.5', '--start-paused', '--disable-keyboard-controls',
        ])

    @unittest.skipUnless(HAS_ROS, 'source ROS 2 to test real bag serialization')
    def test_mcap_and_sqlite_round_trip(self):
        import rosbag2_py
        from rclpy.serialization import deserialize_message
        from rosidl_runtime_py.utilities import get_message

        rows = [
            sample(1, event='seed', command_accepted=1, integrated=0),
            sample(2, qw=math.sqrt(0.5), qz=math.sqrt(0.5), vx_m_s=2),
            sample(3, sample_accepted=0, integrated=0, ax_m_s2=float('nan')),
            sample(4, event='seed', generation=2, command_accepted=1, integrated=0),
            sample(5, generation=2),
        ]
        self.log.write_text(log_text(rows) + log_text([sample(1)]))
        for storage in ('mcap', 'sqlite3'):
            with self.subTest(storage=storage):
                bag = self.root / storage
                result = converter.convert([self.log], bag, storage=storage, position='velocity')
                self.assertEqual(result['records'], 6)
                self.assertEqual(result['poses'], 5)
                self.assertEqual(len(result['segments']), 3)
                self.assertTrue((bag / 'metadata.yaml').is_file())
                reader = rosbag2_py.SequentialReader()
                reader.open(rosbag2_py.StorageOptions(uri=str(bag), storage_id=storage),
                            rosbag2_py.ConverterOptions('cdr', 'cdr'))
                types = {topic.name: get_message(topic.type)
                         for topic in reader.get_all_topics_and_types()}
                if storage == 'mcap':
                    definitions = reader.get_all_message_definitions()
                    self.assertTrue(any(
                        definition.topic_type == 'tf2_msgs/msg/TFMessage'
                        and 'TransformStamped' in definition.encoded_message_definition
                        for definition in definitions
                    ))
                messages = {}
                last_stamp = 0
                while reader.has_next():
                    topic, data, stamp = reader.read_next()
                    self.assertGreaterEqual(stamp, last_stamp)
                    last_stamp = stamp
                    message = deserialize_message(data, types[topic])
                    messages.setdefault(topic, []).append(message)
                    if hasattr(message, 'header'):
                        self.assertEqual(message.header.stamp.sec * 1_000_000_000
                                         + message.header.stamp.nanosec, stamp)
                self.assertEqual(len(messages['/imu/raw']), 4)
                self.assertEqual(len(messages['/imu/status']), 6)
                self.assertTrue(math.isnan(messages['/imu/raw'][1].linear_acceleration.x))
                self.assertEqual(messages['/imu/raw'][0].orientation_covariance[0], -1)
                self.assertEqual(messages['/imu/raw_counts/acceleration'][0].vector.x, -123)
                transforms = messages['/tf']
                self.assertEqual(transforms[0].transforms[0].header.frame_id, 'world')
                rotated = transforms[1].transforms[1]
                self.assertEqual(rotated.header.frame_id, 'imu_reference_s1_g1')
                self.assertEqual(rotated.child_frame_id, 'imu_link_s1_g1')
                self.assertAlmostEqual(rotated.transform.rotation.z, math.sqrt(0.5))
                self.assertAlmostEqual(rotated.transform.translation.x, 0.01)
                self.assertEqual(transforms[2].transforms[1].child_frame_id, 'imu_link_s1_g2')
                self.assertEqual(transforms[-1].transforms[1].child_frame_id, 'imu_link_s2_g1')
                self.assertEqual(transforms[2].transforms[1].transform.translation.x, 0)
                details = {entry.key: entry.value
                           for entry in messages['/imu/status'][0].status[0].values}
                self.assertEqual(details['recorded_ns'], str(rows[0]['recorded_ns']))
                self.assertEqual(details['source_line'], '3')
                reader.close()
                with self.assertRaisesRegex(ValueError, 'already exists'):
                    converter.convert([self.log], bag)

    @unittest.skipUnless(HAS_ROS, 'source ROS 2 to test conversion cleanup')
    def test_filters_and_failed_conversion_cleanup(self):
        self.log.write_text(log_text([sample(1), sample(2, generation=2)]))
        bag = self.root / 'selected'
        result = converter.convert([self.log], bag, generation=2)
        self.assertEqual(result['records'], 1)
        self.log.write_text(log_text([sample(1)]) + 'sample,incomplete\n')
        broken = self.root / 'broken'
        with self.assertRaises(ValueError):
            converter.convert([self.log], broken)
        self.assertFalse(broken.exists())
        self.assertEqual(list(self.root.glob('.imu-bag-*')), [])


if __name__ == '__main__':
    unittest.main()
