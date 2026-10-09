import importlib
import os

NAME = os.environ.get('POP2_VERSION', 'v11')
try:
    V = importlib.import_module('versions.' + NAME)
except ImportError:
    raise SystemExit('POP2_VERSION=%s: no recomp/versions/%s.py' % (NAME, NAME))
