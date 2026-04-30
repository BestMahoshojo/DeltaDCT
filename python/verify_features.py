#!/usr/bin/env python3
import os
import sys


def _import_pydeltadct() -> object:
    python_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(python_dir)
    py_tag = f"py{sys.version_info.major}{sys.version_info.minor}"
    build_candidates = [
        os.path.join(project_dir, f"build_{py_tag}"),
        os.path.join(project_dir, "build"),
        os.path.join(project_dir, "build_py311"),
    ]

    for build_dir in build_candidates:
        if os.path.isdir(build_dir) and build_dir not in sys.path:
            sys.path.insert(0, build_dir)

    try:
        return __import__("pydeltadct")
    except ImportError as exc:
        raise SystemExit(f"Failed to import pydeltadct: {exc}")


def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: verify_features.py <image_a.jpg> <image_b.jpg>")
        return 2

    img_a = os.path.abspath(sys.argv[1])
    img_b = os.path.abspath(sys.argv[2])
    if not os.path.isfile(img_a):
        print(f"Missing image: {img_a}")
        return 2
    if not os.path.isfile(img_b):
        print(f"Missing image: {img_b}")
        return 2

    pydeltadct = _import_pydeltadct()

    features_a = pydeltadct.extract_features(img_a, 2, 10)
    features_b = pydeltadct.extract_features(img_b, 2, 10)

    print("Image A features:", features_a)
    print("Image B features:", features_b)

    if len(features_a) != 10 or len(features_b) != 10:
        raise AssertionError("Expected exactly 10 features per image.")

    overlap = set(features_a).intersection(features_b)
    if overlap:
        raise AssertionError(f"Feature overlap detected: {sorted(overlap)}")

    print("OK: No overlap between the two feature sets.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
