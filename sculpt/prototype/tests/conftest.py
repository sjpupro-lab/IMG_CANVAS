import os
import sys

PROTO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if PROTO_ROOT not in sys.path:
    sys.path.insert(0, PROTO_ROOT)
