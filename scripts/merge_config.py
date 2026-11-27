#!/usr/bin/env python3
import os
from pathlib import Path
import re
import sys
import tempfile


SECTION = re.compile(r"^\s*\[([^\[\]]+)\]\s*(?:[#;].*)?$")
SETTING = re.compile(r"^\s*([a-zA-Z0-9_]+)\s*=")


def split_sections(text):
    sections = {"": []}
    current = ""
    for line in text.splitlines(keepends=True):
        header = SECTION.match(line)
        if header:
            current = header[1].strip()
            if current in sections:
                raise ValueError(f"Repeated config section [{current}]")
            sections[current] = []
        sections[current].append(line if line.endswith("\n") else line + "\n")
    return sections


def _insert_before_trailing_blanks(lines, line):
    index = len(lines)
    while index > 0 and not lines[index - 1].strip():
        index -= 1
    lines.insert(index, line)


def _fsync_dir(directory):
    try:
        flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
        dir_fd = os.open(directory, flags)
        try:
            os.fsync(dir_fd)
        finally:
            os.close(dir_fd)
    except OSError:
        pass


def merge_configs(template_path, user_path, output_path, overrides=None):
    temporary = None
    try:
        template = split_sections(Path(template_path).read_text(encoding="utf-8"))
        source = Path(user_path)
        original = source.stat() if source.exists() else None
        user = split_sections(source.read_text(encoding="utf-8")) if original else {}
        overrides = overrides or {}

        # Legacy files wrote these settings under [slideshow].
        # Keep their values before adding the canonical defaults.
        slideshow = {
            match[1]: line.split("=", 1)[1].strip()
            for line in user.get("slideshow", ())
            if (match := SETTING.match(line))
        }
        legacy_brightness = {
            key: slideshow["brightness_" + key]
            for key in ("auto", "auto_min", "auto_max")
            if "brightness_" + key in slideshow
        }

        for section, defaults in template.items():
            values = overrides.get(section, {})
            is_new_section = section not in user
            if is_new_section:
                # Preserve template comments and ordering for newly added sections.
                lines = defaults.copy()
                user[section] = lines
            else:
                lines = user[section]
            found = set()
            for index, line in enumerate(lines):
                if match := SETTING.match(line):
                    key = match[1]
                    found.add(key)
                    if key in values:
                        lines[index] = f"{key} = {values[key]}\n"
                    elif is_new_section and section == "brightness" and key in legacy_brightness:
                        lines[index] = f"{key} = {legacy_brightness[key]}\n"
            if not is_new_section:
                for line in defaults:
                    match = SETTING.match(line)
                    if not match or match[1] in found:
                        continue
                    key = match[1]
                    found.add(key)
                    if key in values:
                        line = f"{key} = {values[key]}\n"
                    elif section == "brightness" and key in legacy_brightness:
                        line = f"{key} = {legacy_brightness[key]}\n"
                    _insert_before_trailing_blanks(lines, line)
            for key, value in values.items():
                if key not in found:
                    _insert_before_trailing_blanks(lines, f"{key} = {value}\n")

        for section, values in overrides.items():
            if section in template:
                continue
            lines = user.setdefault(section, [f"[{section}]\n"] if section else [])
            found = set()
            for index, line in enumerate(lines):
                if match := SETTING.match(line):
                    key = match[1]
                    found.add(key)
                    if key in values:
                        lines[index] = f"{key} = {values[key]}\n"
            for key, value in values.items():
                if key not in found:
                    _insert_before_trailing_blanks(lines, f"{key} = {value}\n")

        destination = Path(output_path)
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", dir=destination.parent,
            prefix=f".{destination.name}.", delete=False,
        ) as output:
            temporary = output.name
            if original and os.geteuid() == 0:
                os.fchown(output.fileno(), original.st_uid, original.st_gid)
            previous_blank = True
            for section, lines in user.items():
                if section and not previous_blank:
                    output.write("\n")
                output.writelines(lines)
                if lines:
                    previous_blank = not lines[-1].strip()
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, destination)
        temporary = None
        _fsync_dir(destination.parent)
        return True
    except (OSError, UnicodeError, ValueError) as error:
        print(f"Config merge failed: {error}", file=sys.stderr)
        return False
    finally:
        if temporary is not None:
            Path(temporary).unlink(missing_ok=True)


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        sys.exit("Usage: merge_config.py TEMPLATE USER [OUTPUT]")
    sys.exit(0 if merge_configs(*sys.argv[1:3], sys.argv[-1]) else 1)
