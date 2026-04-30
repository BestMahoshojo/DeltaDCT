#!/usr/bin/env python3
import argparse
import sys

import pydeltadct


def main() -> int:
    parser = argparse.ArgumentParser(description="Minimal pydeltadct binding check")
    parser.add_argument("--base", required=True, help="Base JPEG path")
    parser.add_argument("--target", required=True, help="Target JPEG path")
    parser.add_argument(
        "--output-ddct",
        default="binding_test.ddct",
        help="Output .ddct package path",
    )
    parser.add_argument(
        "--output-jpg",
        default="binding_test.restored.jpg",
        help="Output restored JPEG path",
    )
    args = parser.parse_args()

    result = pydeltadct.compress_image(args.target, args.base, args.output_ddct)
    print("compress.success:", result.success)
    print("compress.compression_ratio:", result.compression_ratio)
    print("compress.original_size:", result.original_size)
    print("compress.compressed_size:", result.compressed_size)
    print("compress.time_ms:", result.time_ms)

    if not result.success:
        print("compress_image failed", file=sys.stderr)
        return 1

    ok = pydeltadct.decompress_image(args.output_ddct, args.base, args.output_jpg)
    print("decompress.success:", ok)
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
