#!/usr/bin/env python3
"""
patch_config.py  --  Injects s3ceq daemons into BOINC config.xml

Usage: python3 patch_config.py /path/to/config.xml
"""
import sys
import re
from pathlib import Path

DAEMON_BLOCK = """\
    <daemon>
        <cmd>python3 /srv/s3ceq/work_generator.py</cmd>
        <output>work_generator.log</output>
        <pid_file>work_generator.pid</pid_file>
    </daemon>
    <daemon>
        <cmd>python3 /srv/s3ceq/assimilator.py</cmd>
        <output>assimilator.log</output>
        <pid_file>assimilator.pid</pid_file>
    </daemon>
    <daemon>
        <cmd>python3 /srv/s3ceq/validator.py</cmd>
        <output>validator.log</output>
        <pid_file>validator.pid</pid_file>
    </daemon>
"""

FEEDER_BLOCK = """\
    <one_result_per_user_per_wu>1</one_result_per_user_per_wu>
    <min_sendwork_interval>5</min_sendwork_interval>
    <max_wus_in_progress>100</max_wus_in_progress>
    <max_wus_to_send>50</max_wus_to_send>
"""

if len(sys.argv) < 2:
    print("Usage: patch_config.py <config.xml>")
    sys.exit(1)

config_path = Path(sys.argv[1])
content = config_path.read_text()

# Inject daemons before </daemons>
if "work_generator" not in content:
    content = content.replace("</daemons>", DAEMON_BLOCK + "</daemons>")
    print("[patch] Injected daemon entries")
else:
    print("[patch] Daemons already present, skipping")

# Inject scheduler config before </config>
if "min_sendwork_interval" not in content:
    content = content.replace("</config>", FEEDER_BLOCK + "</config>")
    print("[patch] Injected scheduler config")

config_path.write_text(content)
print(f"[patch] Config written to {config_path}")
