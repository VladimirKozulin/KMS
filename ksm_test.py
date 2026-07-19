import mmap
import time
import os
import sys

# Выделяем 10 MB анонимной памяти
SIZE = 10 * 1024 * 1024 
print(f"Process PID {os.getpid()}: Allocating {SIZE / 1024 / 1024} MB of duplicate RAM...")

# Создаем отображение памяти
mm = mmap.mmap(-1, SIZE, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS, prot=mmap.PROT_READ | mmap.PROT_WRITE)

# Заполняем память одинаковыми байтами (символ 'A')
mm.write(b"A" * SIZE)

# Помечаем эту память флагом MADV_MERGEABLE (значение 12 в Linux)
MADV_MERGEABLE = 12
try:
    mm.madvise(MADV_MERGEABLE)
    print("[SUCCESS] Memory marked as MADV_MERGEABLE. KSM will now scan this region.")
except Exception as e:
    print(f"[FAIL] Could not call madvise: {e}", file=sys.stderr)

print("Keeping process alive. Press Ctrl+C to terminate...")
try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    print("Exiting...")
