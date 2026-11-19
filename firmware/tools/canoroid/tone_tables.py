# SPDX-License-Identifier: GPL-2.0-only

def tone_table(sources, tone_name):
    rows = [line.split() for line in sources.read(tone_name + ".cube").splitlines()
            if line[:1].isdigit()]
    assert len(rows) == 33**3
    return bytes(round(float(v)*255) for b in range(0,33,2)
                  for g in range(0,33,2) for r in range(0,33,2)
                  for v in rows[(b*33+g)*33+r])
