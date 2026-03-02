#!/usr/bin/env python3
"""
MoonLight C++ custom lint rules.

Rules checked:
  no-string-member  : Node class member variables must not use String type.
                      Use Char<N> or char[N] instead.
                      (String can crash when the node is in PSRAM and String is in heap)
  no-static-member  : Node class non-const static data members are forbidden.
                      Multiple instances of the same node must not share state.
  no-serial-print   : Serial.print/printf/println must not be used in src/ files.
                      Use EXT_LOGD/I/W/E(TAG, ...) instead.
                      Exempt lines ending with  // ok-lint

Usage:
  python3 ci/cpp_lint.py [--src <dir>]   (default: src)
  Exits with code 1 if any violation is found.
"""

import argparse
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Patterns
# ---------------------------------------------------------------------------

# Matches a class or struct opening line, capturing the name.
# e.g.  "class Foo : public Node {"  or  "struct Bar {"
RE_CLASS_OPEN = re.compile(r'^\s*(?:class|struct)\s+(\w+)')

# Matches a line that closes a brace.
RE_BRACE_OPEN  = re.compile(r'\{')
RE_BRACE_CLOSE = re.compile(r'\}')

# --- no-string-member -------------------------------------------------------
# A class member of type String (not inside a method body, not a local var).
# We look for the *word* String (not std::string, not substring) as the type,
# followed by an identifier, optional array brackets, and a ; or =.
# Exclude: "const String" return types on method signatures (those have '(')
RE_STRING_MEMBER = re.compile(
    r'^\s{0,3}'               # up to 3 spaces of leading indent (member level)
    r'(?:static\s+)?'         # optional static
    r'(?:const\s+)?'          # optional const
    r'String\s+'              # String type
    r'\w+'                    # variable name
    r'(?:\s*\[.*?\])?'        # optional array brackets
    r'\s*(?:=|;)',             # initializer or end of declaration
)

# --- no-static-member -------------------------------------------------------
# A non-const, non-constexpr static data member.
# Static *methods* have '(' on the same line; we exclude those.
# We also exclude 'static const' and 'static constexpr' (constants are fine).
RE_STATIC_MEMBER = re.compile(
    r'^\s{0,3}'                # up to 3 spaces of leading indent (member level)
    r'static\s+'               # static keyword
    r'(?!'                     # negative lookahead: NOT followed by...
    r'const\b|constexpr\b'    # const/constexpr (constants ok)
    r')',
)

# --- no-serial-print --------------------------------------------------------
RE_SERIAL_PRINT = re.compile(r'\bSerial\.(print|println|printf|write)\b')

# Exemption marker
RE_OK_LINT = re.compile(r'//\s*ok-lint')

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def count_braces(line: str) -> int:
    """Return net brace depth change for a line (open - close)."""
    # Strip string literals and comments to avoid counting braces inside them.
    # Simple approach: remove single-line strings and // comments.
    stripped = re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)   # string literals
    stripped = re.sub(r"'(?:[^'\\]|\\.)*'", "''", stripped)  # char literals
    stripped = re.sub(r'//.*', '', stripped)                # line comments
    opens  = stripped.count('{')
    closes = stripped.count('}')
    return opens - closes


def is_node_subclass(class_stack: list) -> bool:
    """Return True if we are currently inside a Node subclass."""
    # We tag each frame with whether it's a Node subclass.
    for frame in class_stack:
        if frame.get('is_node'):
            return True
    return False


def check_file(path: Path) -> list:
    """Return a list of (lineno, rule, message) tuples for violations."""
    violations = []
    lines = path.read_text(encoding='utf-8', errors='replace').splitlines()

    # Stack frames: {'name': str, 'depth_at_open': int, 'is_node': bool}
    class_stack: list = []
    brace_depth = 0          # absolute brace depth

    for lineno, line in enumerate(lines, start=1):
        # ------------------------------------------------------------------
        # Track class/struct openings BEFORE updating brace_depth
        # ------------------------------------------------------------------
        m = RE_CLASS_OPEN.match(line)
        if m:
            class_name = m.group(1)
            # Determine if this inherits from Node (look for ': public Node'
            # or ': Node' or multiple bases containing Node).
            is_node = bool(re.search(r':\s*(?:public\s+)?(?:\w+,\s*)*Node\b', line))
            # The class body starts at the '{' on this line (or later).
            # We record the depth *before* this line's braces are counted.
            class_stack.append({
                'name': class_name,
                'depth_at_open': brace_depth,
                'is_node': is_node,
            })

        # ------------------------------------------------------------------
        # Update brace depth
        # ------------------------------------------------------------------
        delta = count_braces(line)
        brace_depth += delta

        # Pop closed classes
        while class_stack and brace_depth <= class_stack[-1]['depth_at_open']:
            class_stack.pop()

        # ------------------------------------------------------------------
        # Check rules only at "member level" inside a Node subclass
        # ------------------------------------------------------------------
        # Member level = exactly one brace level deeper than the class open.
        in_node = False
        member_depth = -1
        for frame in reversed(class_stack):
            if frame['is_node']:
                member_depth = frame['depth_at_open'] + 1
                in_node = True
                break

        at_member_level = in_node and (brace_depth == member_depth)

        if at_member_level:
            # no-string-member
            if RE_STRING_MEMBER.match(line):
                # Skip method signatures (they have '(' on the same line)
                if '(' not in line:
                    violations.append((path, lineno, 'no-string-member',
                        f'String member variable in Node class — use Char<N> or char[N] instead: {line.strip()}'))

            # no-static-member
            if RE_STATIC_MEMBER.match(line):
                # Skip static methods (they have '(' on the same line)
                if '(' not in line:
                    violations.append((path, lineno, 'no-static-member',
                        f'Static data member in Node class — multiple instances must not share state: {line.strip()}'))

        # ------------------------------------------------------------------
        # no-serial-print  (whole file, not just Node classes)
        # Only check the non-comment portion of the line.
        # ------------------------------------------------------------------
        line_no_comment = re.sub(r'//.*', '', line)
        line_no_comment = re.sub(r'"(?:[^"\\]|\\.)*"', '""', line_no_comment)
        line_no_comment = re.sub(r"'(?:[^'\\]|\\.)*'", "''", line_no_comment)
        if RE_SERIAL_PRINT.search(line_no_comment):
            if not RE_OK_LINT.search(line):
                violations.append((path, lineno, 'no-serial-print',
                    f'Serial.print* in src/ — use EXT_LOG*(TAG, ...) instead: {line.strip()}'))

    return violations


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description='MoonLight C++ custom lint rules')
    parser.add_argument('--src', default='src', help='Source directory to scan (default: src)')
    args = parser.parse_args()

    src_root = Path(args.src)
    if not src_root.is_dir():
        print(f'ERROR: source directory not found: {src_root}', file=sys.stderr)
        return 2

    all_violations: list = []
    for ext in ('*.cpp', '*.h'):
        for path in sorted(src_root.rglob(ext)):
            all_violations.extend(check_file(path))

    if all_violations:
        for path, lineno, rule, message in all_violations:
            print(f'{path}:{lineno}: [{rule}] {message}')
        print(f'\n{len(all_violations)} violation(s) found.', file=sys.stderr)
        return 1

    print(f'cpp_lint: OK ({sum(1 for _ in src_root.rglob("*.h")) + sum(1 for _ in src_root.rglob("*.cpp"))} files checked, 0 violations)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
