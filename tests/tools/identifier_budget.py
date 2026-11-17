"""Measure saved trials, including their archived helper model contexts.

After is a counterfactual replay on the SAME tool results/arguments/citations,
not another model run or a token measurement. Product code has no alias table.
Only typed references change; file bytes and integrity digests remain opaque.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import sqlite3
from urllib.parse import parse_qs, unquote

ID_KEYS = {'id', 'run_id', 'project_id', 'conversation_id', 'op_id', 'call_id',
           'tool_use_id', 'environment_id', 'artifact_id', 'execution_job_id',
           'comment_id', 'note_id', 'group_id', 'parent_call', 'invocation', 'task',
           'activation', 'rev', 'base_rev', 'at_rev', 'execution_rev'}
CARD_KEYS = {'card', 'card_id', 'from_card', 'to_card', 'segments',
             'read_cards', 'write_cards', 'run_cards'}
DOCUMENTS = {'project_json', 'layout_json', 'card_json', 'meta_json', 'json'}
OPAQUE = {'bytes', 'stdout_bytes', 'stderr_bytes', 'text', 'record_json'} | DOCUMENTS
UUID = re.compile(r'\b[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\b')


def identifier_chars(value, key=''):
    if isinstance(value, dict):
        return sum(identifier_chars(v, k) for k, v in value.items())
    if isinstance(value, list):
        return sum(identifier_chars(v, key) for v in value)
    if not isinstance(value, str):
        return len(str(value)) if key in ID_KEYS and isinstance(value, int) else 0
    if key in ID_KEYS | CARD_KEYS | {'commit', 'commit_id', 'head', 'parents'}:
        return len(value)
    if key in DOCUMENTS:
        try:
            return identifier_chars(json.loads(value))
        except (ValueError, TypeError):
            pass
    return sum(len(m[0]) for m in UUID.finditer(value))


def citation_chars(text):
    total = 0
    for match in re.finditer(r'(card|run|log|artifact):([^\s)]+)', text):
        target, _, query = match[2].partition('?')
        target, _, invocation = target.partition('#')
        total += len(unquote(target).replace('/', '')) + len(invocation)
        args = parse_qs(query)
        for key in ('id', 'invocation'):
            total += sum(len(v) for v in args.get(key, []))
    plain = re.sub(r'(card|run|log|artifact):[^\s)]+', '', text)
    return total + sum(len(m[0]) for m in UUID.finditer(plain))


def old_operation(conversation, call):
    h = hashlib.sha256((conversation + ':' + call).encode()).hexdigest()
    return h[:8] + '-' + h[8:12] + '-4' + h[13:16] + '-8' + h[17:20] + '-' + h[20:32]


class Replay:
    def __init__(self, conversation):
        self.conversation = conversation
        self.maps = collections.defaultdict(dict)
        self.calls = {}
        self.full_calls = {}
        self.operations = {}
        self.local_calls = {}
        self.current_call = '1'
        self.scope = ''

    def index(self, messages, old_scope='', scope=''):
        calls = {}
        for message in messages:
            for block in message.get('content', []):
                if block.get('type') != 'tool_use':
                    continue
                original = block['id']
                if original in calls:
                    raise ValueError('Repeated saved call ID in one model context')
                local = str(len(calls) + 1)
                calls[original] = local
                self.calls[original] = local
                full = scope + local
                self.full_calls[old_scope + original] = full
                self.operations[old_operation(self.conversation, old_scope + original)] = 'abcdef-' + full
        return calls

    def reference(self, kind, value):
        if not value:
            return value
        scope = self.maps[kind]
        if value not in scope:
            scope[value] = str(len(scope) + 1)
        return scope[value]

    def text(self, value):
        for scope in self.maps.values():
            for old, new in scope.items():
                if UUID.fullmatch(old):
                    value = value.replace(old, new)
        return value

    def rewrite(self, value, key=''):
        if isinstance(value, dict):
            out = {k: self.rewrite(v, k) for k, v in value.items() if k != 'execution_job_id'}
            if 'author' in out and 'message' in out and len(str(out.get('id', ''))) == 40:
                out['id'] = out['id'][:12]
            return out
        if isinstance(value, list):
            return [self.rewrite(v, key) for v in value]
        if not isinstance(value, str) or not value:
            return value
        if key in OPAQUE:
            return value
        if key in {'id', 'call_id', 'tool_use_id', 'parent_call'}:
            if value in self.local_calls:
                return self.local_calls[value]
            if value in self.full_calls:
                return self.full_calls[value]
            if value in self.calls:
                return self.calls[value]
        if key in {'run_id', 'environment_id'}:
            return self.reference(key, value)
        if key in {'project_id', 'conversation_id'}:
            return self.maps[key].setdefault(value, 'abcdef')
        if key == 'op_id':
            return self.operations.setdefault(value, 'abcdef-' + self.scope + self.current_call)
        if key in {'commit', 'commit_id', 'head', 'parents'} and re.fullmatch('[0-9a-f]{40}', value):
            return value[:12]
        return value


def contexts(db, conversation):
    archived = []
    for raw, in db.execute('SELECT messages FROM archives WHERE conversation=? ORDER BY rowid', (conversation,)):
        archived += json.loads(raw)
    raw = db.execute('SELECT messages FROM conversations WHERE id=?', (conversation,)).fetchone()[0]
    main = archived + json.loads(raw)
    helpers = []
    # The fixture schema predates or includes helper archives depending on trial.
    columns = {row[1] for row in db.execute('PRAGMA table_info(helpers)')}
    query = 'SELECT parent_call,task,messages' + (',archive' if 'archive' in columns else '') + ' FROM helpers WHERE conversation=? ORDER BY rowid'
    for row in db.execute(query, (conversation,)):
        trace = (json.loads(row[3]) if len(row) > 3 and row[3] else []) + json.loads(row[2])
        helpers.append((row[0], row[1], trace))
    return main, helpers


def measure(path, conversation):
    db = sqlite3.connect('file:' + str(path) + '?mode=ro', uri=True)
    main, helpers = contexts(db, conversation)
    db.close()
    replay = Replay(conversation)
    all_contexts = [(main, replay.index(main), '')]
    pending = helpers[:]
    while pending:
        progress = False
        for parent, task, messages in pending[:]:
            if parent not in replay.full_calls:
                continue
            old_scope = hashlib.sha256((parent + ':' + str(task)).encode()).hexdigest() + ':'
            scope = replay.full_calls[parent] + '.' + str(task) + '.'
            all_contexts.append((messages, replay.index(messages, old_scope, scope), scope))
            pending.remove((parent, task, messages))
            progress = True
        if not progress:
            raise ValueError('Helper parent is absent from the saved traces: ' + str(path))
    counts = collections.Counter()
    for messages, calls, scope in all_contexts:
        replay.local_calls = calls
        replay.scope = scope
        for message in messages:
            for block in message.get('content', []):
                kind = block.get('type')
                if kind == 'tool_use':
                    counts['calls'] += 1
                    counts['call_chars_before'] += 2 * len(block['id'])
                    counts['call_chars_after'] += 2 * len(calls[block['id']])
                    replay.current_call = calls[block['id']]
                    arguments = json.loads(block.get('input', '{}'))
                    counts['argument_chars_before'] += identifier_chars(arguments)
                    counts['argument_chars_after'] += identifier_chars(replay.rewrite(arguments))
                if message.get('role') == 'assistant' and kind == 'text':
                    text = block.get('text', '')
                    counts['citation_chars_before'] += citation_chars(text)
                    counts['citation_chars_after'] += citation_chars(replay.text(text))
                if kind != 'tool_result':
                    continue
                replay.current_call = calls.get(block.get('tool_use_id'), '1')
                raw = block.get('content', '')
                if not isinstance(raw, str):
                    continue
                counts['tool_bytes_before'] += len(raw.encode())
                try:
                    result = json.loads(raw)
                except ValueError:
                    n = sum(len(m[0]) for m in UUID.finditer(raw))
                    counts['identifier_chars_before'] += n
                    counts['identifier_chars_after'] += n
                    continue
                counts['identifier_chars_before'] += identifier_chars(result)
                counts['identifier_chars_after'] += identifier_chars(replay.rewrite(result))
    return dict(conversation=conversation, contexts=len(all_contexts), **counts)


def discover(root):
    found = {}
    ranks = {}
    wanted = {}
    archive_roots = set()
    for summary in (root / 'runs3').rglob('summary.txt'):
        for match in re.finditer(r'== conversation ([0-9a-f-]+)', summary.read_text()):
            wanted[match[1]] = str(summary.parent.relative_to(root))
        reset = summary.parent / 'reset.txt'
        if reset.exists():
            for match in re.finditer(r'^reset (.*?) from ', reset.read_text(), re.M):
                project = Path(match[1])
                if project.is_relative_to(root):
                    archive_roots.add(project.parent)
    candidates = list((root / 'runs2').rglob('*.sqlite')) + list((root / 'runs3').rglob('*.sqlite'))
    for archive in sorted(archive_roots):
        candidates += list(archive.rglob('*.sqlite'))
    for path in candidates:
        db = sqlite3.connect('file:' + str(path) + '?mode=ro', uri=True)
        try:
            if db.execute("SELECT 1 FROM sqlite_master WHERE name='conversations'").fetchone():
                for cid, updated, messages in db.execute('SELECT id,updated,messages FROM conversations ORDER BY id'):
                    direct = path.is_relative_to(root / 'runs2') or path.is_relative_to(root / 'runs3')
                    if (direct or cid in wanted) and (cid not in found or (updated, len(messages)) > ranks[cid]):
                        trial = wanted.get(cid, '/'.join(path.relative_to(root).parts[:2]))
                        found[cid] = (path, trial)
                        ranks[cid] = (updated, len(messages))
        finally:
            db.close()
    missing = set(wanted) - set(found)
    if missing:
        raise ValueError('No complete saved conversation for: ' + str(sorted(missing)))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path, help='tuning directory containing runs2 and runs3')
    args = parser.parse_args()
    result = []
    for cid, (path, trial) in sorted(discover(args.root).items(), key=lambda item: item[1][1]):
        row = measure(path, cid)
        row.update(source=str(path.relative_to(args.root)), trial=trial)
        result.append(row)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
