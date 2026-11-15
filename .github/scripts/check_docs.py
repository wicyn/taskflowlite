"""Check repository-local Markdown/HTML links without accessing the network."""
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[2]


def main():
    paths = [ROOT / 'README.md', ROOT / 'README.en.md']
    for folder in ('documentation', 'examples', 'test', 'benchmarks', 'cmake', '.github'):
        paths.extend((ROOT / folder).rglob('*.md'))
    failures = []
    for path in paths:
        text = path.read_text(encoding='utf-8-sig')
        # Code snippets may contain illustrative links that are not references.
        text = re.sub(r'^```[^\n]*\n.*?^```[^\n]*$', '', text, flags=re.M | re.S)
        links = re.findall(r'\]\(([^\s)]+)(?:\s+"[^"]*")?\)', text)
        links += re.findall(r'(?:src|href)=["\']([^"\']+)["\']', text)
        for link in links:
            parsed = urlsplit(link.strip('<>'))
            if parsed.scheme or parsed.netloc or not parsed.path:
                continue
            target = path.parent / unquote(parsed.path)
            if not target.exists():
                failures.append(f'{path.relative_to(ROOT)}: missing {link}')
    if failures:
        print('\n'.join(failures))
        return 1
    print(f'Checked local links in {len(paths)} Markdown files.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
