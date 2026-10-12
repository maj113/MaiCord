"""Verify published MLS ciphersuite-2 vectors and emit bounded native fixtures."""
from pathlib import Path
import argparse
import hashlib
import hmac
import json
import sys
sys.dont_write_bytecode = True
from dave_key_derivation_oracle import label_expand
from mls_exporter_oracle import export

FIELDS = ['joiner_secret', 'epoch_secret', 'sender_data_secret', 'encryption_secret',
          'exporter_secret', 'epoch_authenticator', 'external_secret', 'confirmation_key',
          'membership_key', 'resumption_psk', 'init_secret', 'welcome_secret', 'confirmation_tag']
LABELS = ['sender data', 'encryption', 'exporter', 'authentication', 'external', 'confirm',
          'membership', 'resumption', 'init']


def finish(joiner, psk, confirmed, context):
    member = hmac.digest(joiner, psk, 'sha256')
    epoch = label_expand(member, b'epoch', context, 32)
    values = [joiner, epoch] + [label_expand(epoch, label.encode(), b'', 32) for label in LABELS]
    values += [label_expand(member, b'welcome', b'', 32), hmac.digest(values[7], confirmed, 'sha256')]
    return dict(zip(FIELDS, values))


def schedule(init, commit, psk, confirmed, context):
    joiner = label_expand(hmac.digest(init, commit, 'sha256'), b'joiner', context, 32)
    return finish(joiner, psk, confirmed, context)


def packed(values):
    return b''.join(values[name] for name in FIELDS)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--vectors', type=Path, required=True)
    args = parser.parse_args()
    raw = args.vectors.read_bytes()
    if hashlib.sha256(raw).hexdigest() != '05aa9a68bd2538ace72d8c53375984cc728ef62220ebf314df675708546d97a7':
        raise ValueError('Expected the pinned MLSWG key-schedule vector file')
    vector = next(v for v in json.loads(raw) if v['cipher_suite'] == 2)
    print('published_source_sha256=' + hashlib.sha256(raw).hexdigest())
    lines = ['// MLSWG cfd450286d1bfd9cd2519b95c80f9771f94a5b1a, ciphersuite 2.',
             '// Verified and emitted by tools/mls_key_schedule_oracle.py.',
             'struct ScheduleVector { const char *init, *commit, *psk, *confirmed, *context, *expected, *label, *export_context, *exported, *external_pub; };',
             'constexpr ScheduleVector schedule_vectors[]{']
    init = bytes.fromhex(vector['initial_init_secret'])
    for item in vector['epochs']:
        commit, psk, confirmed, context = [bytes.fromhex(item[k]) for k in
            ('commit_secret', 'psk_secret', 'confirmed_transcript_hash', 'group_context')]
        values = schedule(init, commit, psk, confirmed, context)
        for field in FIELDS:
            if field in item:
                assert values[field].hex() == item[field], field
        exporter = item['exporter']
        label = exporter['label'].encode()
        exported = export(values['exporter_secret'], label, bytes.fromhex(exporter['context']), exporter['length'])
        assert exported.hex() == exporter['secret'], 'published exporter'
        fields = [init.hex(), commit.hex(), psk.hex(), confirmed.hex(), context.hex(), packed(values).hex(),
                  label.hex(), exporter['context'], exporter['secret'], item['external_pub']]
        lines.append('    {' + ', '.join('"' + value + '"' for value in fields) + '},')
        init = values['init_secret']
    lines.append('};')
    init = bytes(range(32))
    initial = schedule(init, bytes(32), bytes(32), b'', b'initial synthetic context')
    lines.append('constexpr char initial_schedule[] = "' + packed(initial).hex() + '";')
    interim = hashlib.sha256(bytes([32]) + initial['confirmation_tag']).digest()
    confirmed = hashlib.sha256(interim + b'validated confirmed input').digest()
    following = hashlib.sha256(confirmed + bytes([32]) + initial['confirmation_tag']).digest()
    for name, value in [('initial_interim', interim), ('next_confirmed', confirmed), ('next_interim', following)]:
        lines.append(f'constexpr char {name}[] = "{value.hex()}";')
    digest = hashlib.sha256()
    for size in (0, 1, 55, 56, 63, 64, 65, 16383, 16384):
        context = bytes((i * 17 + 3) & 255 for i in range(size))
        digest.update(packed(schedule(init, bytes(32), bytes(32), b'', context)))
        digest.update(packed(schedule(init, bytes(range(32)), bytes(32), bytes(range(32)), context)))
        digest.update(packed(finish(init, bytes(range(32)), bytes(range(32)), context)))
    lines.append(f'constexpr char schedule_matrix_digest[] = "{digest.hexdigest()}";\n')
    (Path(__file__).resolve().parents[1] / 'tests/data/mls_schedule_vectors.hpp').write_text('\n'.join(lines))
    print(f'published_epochs={len(vector["epochs"])} boundary_cases=27 sha256={digest.hexdigest()}')


if __name__ == '__main__':
    main()
