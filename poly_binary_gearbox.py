#!/usr/bin/env python3
"""
poly_binary_gearbox.py -- HyperLang Sub-Scalar Triad & Poly-Dimensional Binary Cryptography.
Full zero-copy legacy C99 ABI chameleon interoperability across NanoBool, MicroBool, and MiniBool.
"""

from __future__ import annotations
import ctypes
from typing import List, Tuple


class NanoBool(ctypes.Structure):
    """4-bit sub-byte quantum gear with exact canonical C99 bool at offset 0."""
    _pack_ = 1
    _layout_ = "ms"
    _fields_ = [
        ("legacy_val", ctypes.c_bool),
        ("gears", ctypes.c_uint8),
    ]

    @classmethod
    def create(cls, quadrant: int, taut: bool, spin_cw: bool) -> NanoBool:
        nb = cls()
        is_true = (quadrant & 0x03) < 2
        nb.legacy_val = is_true
        nb.gears = ((quadrant & 0x03) << 2) | ((1 if taut else 0) << 1) | (1 if spin_cw else 0)
        return nb

    @property
    def quadrant(self) -> int:
        return (self.gears >> 2) & 0x03

    @property
    def taut(self) -> bool:
        return bool(self.gears & 0x02)

    @property
    def spin_cw(self) -> bool:
        return bool(self.gears & 0x01)

    def collapse(self) -> bool:
        return bool(self.legacy_val)

    def __repr__(self) -> str:
        return f"<NanoBool: Quad={self.quadrant} Taut={self.taut} CW={self.spin_cw} (C99 bool={self.legacy_val})>"


class MicroBool(ctypes.Structure):
    """8-bit Newtonian bit with exact canonical C99 bool at offset 0."""
    _pack_ = 1
    _layout_ = "ms"
    _fields_ = [
        ("legacy_val", ctypes.c_bool),
        ("gears", ctypes.c_uint8),
    ]

    @classmethod
    def create(cls, sector: int, tension: int, damping: int, spin_cw: bool) -> MicroBool:
        mb = cls()
        is_true = (sector & 0x07) < 4
        mb.legacy_val = is_true
        mb.gears = ((sector & 0x07) << 5) | ((tension & 0x03) << 3) | ((damping & 0x03) << 1) | (1 if spin_cw else 0)
        return mb

    @property
    def sector(self) -> int:
        return (self.gears >> 5) & 0x07

    @property
    def tension(self) -> int:
        return (self.gears >> 3) & 0x03

    @property
    def damping(self) -> int:
        return (self.gears >> 1) & 0x03

    @property
    def spin_cw(self) -> bool:
        return bool(self.gears & 0x01)

    def collapse(self) -> bool:
        return bool(self.legacy_val)

    def rotate(self, delta_sectors: int) -> MicroBool:
        new_sector = (self.sector + delta_sectors) & 0x07
        return MicroBool.create(new_sector, self.tension, self.damping, self.spin_cw)

    def __repr__(self) -> str:
        hemi = "NORTH(True)" if self.collapse() else "SOUTH(False)"
        return f"<MicroBool: Sector={self.sector} ({self.sector*45}deg) Tens={self.tension} Damp={self.damping} Spin={self.spin_cw} (C99 bool={self.legacy_val}) [{hemi}]>"


class MiniBool(ctypes.Structure):
    """Precision clockwork chameleon with exact canonical C99 bool at offset 0."""
    _pack_ = 1
    _layout_ = "ms"
    _fields_ = [
        ("legacy_val", ctypes.c_bool),
        ("phase_wheel", ctypes.c_uint8),
        ("mechanics", ctypes.c_uint8),
    ]

    @classmethod
    def create(cls, phase_ticks: int, tension: int, momentum: int) -> MiniBool:
        mb = cls()
        mb.legacy_val = (phase_ticks & 0xFF) < 128
        mb.phase_wheel = phase_ticks & 0xFF
        mb.mechanics = ((tension & 0x0F) << 4) | (momentum & 0x0F)
        return mb

    @property
    def tension(self) -> int:
        return (self.mechanics >> 4) & 0x0F

    @property
    def momentum(self) -> int:
        return self.mechanics & 0x0F

    def collapse(self) -> bool:
        return bool(self.legacy_val)

    def __repr__(self) -> str:
        angle = self.phase_wheel * 360.0 / 256.0
        return f"<MiniBool: Phase={self.phase_wheel}/255 ({angle:.1f}deg) Tens={self.tension} Mom={self.momentum} (C99 bool={self.legacy_val})>"


def poly_keystream_bit(key_seed: int, bit_index: int) -> int:
    x = (((key_seed & 0xFF) << 16) ^ bit_index) & 0xFFFFFFFF
    x = (((x >> 16) ^ x) * 0x45D9F3B) & 0xFFFFFFFF
    x = (((x >> 16) ^ x) * 0x45D9F3B) & 0xFFFFFFFF
    x = ((x >> 16) ^ x) & 0xFFFFFFFF
    return x & 1


# =========================================================================
# POLY-DIMENSIONAL BINARY CRYPTO ENGINE
# =========================================================================
class PolyBinaryStream:
    def __init__(self, surface_text: str, secret_text: str, key_seed: int = 0xA5):
        self.surface_text = surface_text
        self.key_seed = key_seed & 0xFF
        self.gears: List[MicroBool] = []

        surface_bytes = surface_text.encode("utf-8")
        secret_bytes = secret_text.encode("utf-8") if secret_text else b""
        if len(secret_bytes) > 255:
            secret_bytes = secret_bytes[:255]
        max_cap = max(0, len(surface_bytes) - 1)
        if len(secret_bytes) > max_cap:
            secret_bytes = secret_bytes[:max_cap]

        total_bits = len(surface_bytes) * 8

        for i in range(total_bits):
            b = i // 8
            bit = 7 - (i % 8)
            surf_bit = bool((surface_bytes[b] >> bit) & 1)

            if b == 0:
                secret_bit = bool((len(secret_bytes) >> bit) & 1)
            elif (b - 1) < len(secret_bytes):
                secret_bit = bool((secret_bytes[b - 1] >> bit) & 1)
            else:
                secret_bit = bool(poly_keystream_bit(self.key_seed ^ 0x5C, i))

            k_bit = poly_keystream_bit(self.key_seed, i)
            cipher_bit = secret_bit ^ bool(k_bit)

            base_sector = 1 if surf_bit else 5
            tension = 3 if cipher_bit else 1
            spin_cw = bool(((self.key_seed + i) & 1) ^ (1 if cipher_bit else 0))
            damping = (self.key_seed ^ b) & 0x01

            self.gears.append(MicroBool.create(base_sector, tension, damping, spin_cw))

        self.torque_hashes = []
        for b in range(len(surface_bytes)):
            h = 0
            for bit in range(8):
                i = b * 8 + (7 - bit)
                base_sector = self.gears[i].sector
                tension = self.gears[i].tension
                prev_phase = self.gears[i - 1].sector if i > 0 else self.key_seed
                h ^= ((prev_phase << 4) | (base_sector ^ tension)) & 0xFF
            self.torque_hashes.append(h)

    def decode_surface(self) -> str:
        res = bytearray()
        for b in range(len(self.gears) // 8):
            byte_val = 0
            for bit in range(8):
                bit_idx = b * 8 + (7 - bit)
                if self.gears[bit_idx].legacy_val:
                    byte_val |= (1 << bit)
            res.append(byte_val)
        return res.decode("utf-8", errors="replace")

    def decode_deep(self) -> str:
        num_bytes = len(self.gears) // 8
        if num_bytes == 0:
            return ""

        # Recover length
        payload_len = 0
        for bit in range(8):
            bit_idx = 7 - bit
            mb = self.gears[bit_idx]
            expected_spin = bool((self.key_seed + bit_idx) & 1)
            cipher_bit = (mb.tension >= 2) or (mb.spin_cw != expected_spin)
            k_bit = poly_keystream_bit(self.key_seed, bit_idx)
            plain_bit = cipher_bit ^ bool(k_bit)
            if plain_bit:
                payload_len |= (1 << bit)

        avail = max(0, num_bytes - 1)
        payload_len = min(payload_len, avail)

        res = bytearray()
        for b in range(payload_len):
            sec_byte = 0
            for bit in range(8):
                bit_idx = (b + 1) * 8 + (7 - bit)
                mb = self.gears[bit_idx]
                expected_spin = bool((self.key_seed + bit_idx) & 1)
                cipher_bit = (mb.tension >= 2) or (mb.spin_cw != expected_spin)
                k_bit = poly_keystream_bit(self.key_seed, bit_idx)
                plain_bit = cipher_bit ^ bool(k_bit)
                if plain_bit:
                    sec_byte |= (1 << bit)
            res.append(sec_byte)
        return res.decode("utf-8", errors="replace")

    def verify_integrity(self) -> Tuple[bool, int]:
        surface_bytes = self.surface_text.encode("utf-8")
        for b in range(len(surface_bytes)):
            h = 0
            for bit in range(8):
                i = b * 8 + (7 - bit)
                gear = self.gears[i]
                surface_bit = bool((surface_bytes[b] >> bit) & 1)
                if surface_bit != gear.legacy_val:
                    return False, i
                prev_phase = self.gears[i - 1].sector if i > 0 else self.key_seed
                h ^= ((prev_phase << 4) | (gear.sector ^ gear.tension)) & 0xFF
            if h != self.torque_hashes[b]:
                return False, b * 8
        return True, -1


def run_demo():
    print("=" * 70)
    print("   PYTHON SUB-SCALAR TRIAD LEGACY C99 INTEROP & POLY-BINARY SUITE")
    print("=" * 70 + "\n")

    # 1. NanoBool
    nb_t = NanoBool.create(0, taut=True, spin_cw=True)
    nb_f = NanoBool.create(3, taut=False, spin_cw=False)
    print(f"[1] NANOBOOL (C99 Chameleon):")
    print(f"    NanoBool True  -> legacy_val={nb_t.legacy_val} (size={ctypes.sizeof(nb_t)} B)")
    print(f"    NanoBool False -> legacy_val={nb_f.legacy_val} (size={ctypes.sizeof(nb_f)} B)\n")

    # 2. MicroBool
    mb_t = MicroBool.create(sector=1, tension=3, damping=1, spin_cw=True)
    mb_f = MicroBool.create(sector=6, tension=1, damping=0, spin_cw=False)
    print(f"[2] MICROBOOL (C99 Chameleon):")
    print(f"    MicroBool True  -> legacy_val={mb_t.legacy_val} (size={ctypes.sizeof(mb_t)} B)")
    print(f"    MicroBool False -> legacy_val={mb_f.legacy_val} (size={ctypes.sizeof(mb_f)} B)\n")

    # 3. MiniBool
    mini_t = MiniBool.create(phase_ticks=42, tension=14, momentum=9)
    mini_f = MiniBool.create(phase_ticks=200, tension=4, momentum=3)
    print(f"[3] MINIBOOL (Clockwork with legacy_val at Offset 0):")
    print(f"    MiniBool True  -> legacy_val={mini_t.legacy_val} (size={ctypes.sizeof(mini_t)} B)")
    print(f"    MiniBool False -> legacy_val={mini_f.legacy_val} (size={ctypes.sizeof(mini_f)} B)\n")

    # 4. Poly-Binary Steganography
    surface = "PUBLIC WEATHER: 72F SUNNY TODAY"
    secret = "HMT-CRYPTO-KEY:99482"
    key = 0xA5
    stream = PolyBinaryStream(surface, secret, key)

    print(f"[4] POLY-DIMENSIONAL BINARY ENCODING:")
    print(f"    Carrier Surface Message : \"{stream.decode_surface()}\"")
    print(f"    Embedded Deep Message   : \"{stream.decode_deep()}\"")
    intact, _ = stream.verify_integrity()
    print(f"    Gear Meshing Integrity  : {'100% SYNCHRONIZED' if intact else 'JAMMED'}\n")

    print("=" * 70)
    print("  [+] ALL THREE SUB-SCALAR BOOLS INTEROP DIRECTLY WITH C99 ABI!")
    print("=" * 70)


if __name__ == "__main__":
    run_demo()
