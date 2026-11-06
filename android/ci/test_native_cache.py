import json
from pathlib import Path
import struct
import tempfile
import unittest
import native_cache as cache

WORKFLOW = 'env:\n  NDK_VERSION: 28.2.13676358\n' + ''.join(
    '      - name: ' + name + '\n        run: |\n          build ' + name + '\n'
    for name in cache.BUILD_STEPS)
ENTRIES = [('src/main.cpp', '100644', 'a'*40), ('3rdparty/SDL3', '160000', 'b'*40),
           ('CMakeLists.txt', '100644', 'c'*40), ('android/app/Main.kt', '100644', 'd'*40)]

class NativeCacheTests(unittest.TestCase):
    def key(self, entries=ENTRIES, workflow=WORKFLOW, image='ubuntu-image-1'):
        return cache.compatibility_key(entries, workflow, image)

    def test_interface_and_tests_keep_native_payload_compatible(self):
        changed = ENTRIES[:3] + [('android/app/Main.kt', '100644', 'e'*40),
                                 ('tests/Test.cpp', '100644', 'f'*40),
                                 ('docs/status.md', '100644', '0'*40)]
        self.assertEqual(self.key(), self.key(changed))

    def test_native_code_submodule_and_jni_invalidate(self):
        for index in range(3):
            changed = ENTRIES.copy()
            path, mode, _ = changed[index]
            changed[index] = (path, mode, 'f'*40)
            self.assertNotEqual(self.key(), self.key(changed))
        self.assertNotEqual(self.key(), self.key(ENTRIES + [('android/app/native.cpp', '100644', 'a'*40)]))

    def test_ndk_build_options_and_image_invalidate(self):
        for text in (WORKFLOW.replace('28.2.13676358', '29.0'),
                     WORKFLOW.replace('build Configure', 'build Configure -DOTHER=ON')):
            self.assertNotEqual(self.key(), self.key(workflow=text))
        self.assertNotEqual(self.key(), self.key(image='ubuntu-image-2'))
        with self.assertRaises(ValueError):
            self.key(workflow=WORKFLOW.replace('- name: Configure\n', '- name: Missing\n'))

    def test_payload_hash_architecture_and_fingerprint_guards(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            elf = bytearray(64)
            elf[:6] = b'\x7fELF\x02\x01'
            struct.pack_into('<H', elf, 18, 183)
            for relative in cache.LIBRARIES.values():
                target = stage / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(elf)
            manifest = {'compatibility_key': self.key(), 'source_commit': 'a'*40,
                        'libraries': cache.library_hashes(stage)}
            (stage / 'native-cache.json').write_text(json.dumps(manifest))
            self.assertEqual(cache.verify(stage, self.key()), manifest)
            with self.assertRaises(ValueError):
                cache.verify(stage, 'wrong-key')
            (stage / 'libmain.so').write_bytes(elf + b'corrupted')
            with self.assertRaises(ValueError):
                cache.verify(stage, self.key())
            elf[18:20] = (62).to_bytes(2, 'little')
            (stage / 'libmain.so').write_bytes(elf)
            with self.assertRaises(ValueError):
                cache.library_hashes(stage)

if __name__ == '__main__':
    unittest.main()
