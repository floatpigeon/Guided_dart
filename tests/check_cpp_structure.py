#!/usr/bin/env python3
"""Repository structure regression for includable C++ files.

Files group a cohesive responsibility. Related value types, private nested state
and ordinary functions are allowed; compilation and the ODR check verify their
dependencies and inline definitions.
"""

from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def strip_literals(text):
    return re.sub(
        r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: re.sub(r'[^\n]', ' ', match.group()),
        text,
    )


def check():
    listed = subprocess.check_output(
        ['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=ROOT
    ).decode().split('\0')
    paths = sorted({ROOT / name for name in listed if name and (ROOT / name).is_file()})
    errors = []
    count = 0
    for path in paths:
        relative = path.relative_to(ROOT)
        if path.suffix == '.hpp':
            errors.append(f'{relative}: C++ headers are forbidden')
        if path.suffix != '.cpp':
            continue
        count += 1
        text = path.read_text()
        clean = strip_literals(text)
        has_main = bool(re.search(r'\bint\s+main\s*\(', clean))
        namespaces = list(re.finditer(r'\bnamespace\b', clean))
        if len(namespaces) != 1 and not (has_main and not namespaces):
            errors.append(f'{relative}: expected exactly one named namespace')
        elif namespaces:
            match = re.match(r'namespace\s+[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*\s*\{', clean[namespaces[0].start():])
            if not match:
                errors.append(f'{relative}: anonymous namespaces, aliases and using namespace are forbidden')
            prefix = clean[:namespaces[0].start()]
            prefix = re.sub(r'^\s*#.*$', '', prefix, flags=re.MULTILINE)
            if prefix.strip():
                errors.append(f'{relative}: namespace must precede all declarations')
        includes = re.findall(r'^\s*#\s*include\s*[<"]([^">]+)[">]', text, re.MULTILINE)
        if relative.parts[0] == 'src':
            for include in includes:
                if (include.startswith(('sys/', 'camera/k230/', 'spi/'))
                        or include in ('unistd.h', 'fcntl.h', 'pthread.h', 'signal.h', 'dirent.h')
                        or re.match(r'(?:mpi_|k_|drv_).*\.h$', include)):
                    errors.append(f'{relative}: L4 includes a platform implementation header: {include}')
        for include in re.findall(r'^\s*#\s*include\s*"([^"]+)"', text, re.MULTILINE):
            if relative.parts[0] == 'include':
                for base in (path.parent, ROOT, ROOT / 'include/firmware/src'):
                    resolved = (base / include).resolve()
                    if resolved.is_relative_to(ROOT / 'src'):
                        errors.append(f'{relative}: lower-level library depends on L4: {include}')
                        break
            if include.endswith('.hpp'):
                errors.append(f'{relative}: includes a forbidden C++ header: {include}')
            if include.endswith('.cpp') and not any(
                (base / include).is_file()
                for base in (path.parent, ROOT, ROOT / 'include/firmware/src')
            ):
                errors.append(f'{relative}: missing C++ dependency: {include}')
        if not has_main:
            if not re.search(r'^#\s*ifndef\s+\w+\s*\n#\s*define\s+\w+', text):
                errors.append(f'{relative}: includable C++ files need an include guard')
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print(f'C++ structure passed: {count} files, no .hpp, guarded includes and one named namespace per file')
    return 0


if __name__ == '__main__':
    sys.exit(check())
