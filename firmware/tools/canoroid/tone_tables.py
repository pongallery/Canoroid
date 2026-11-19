# SPDX-License-Identifier: GPL-2.0-only

def tone_table(sources, tone_name):
    if tone_name in ("FADED", "VIVID"):
        table = bytearray()
        for b in range(17):
            for g in range(17):
                for r in range(17):
                    rgb = (r/16, g/16, b/16)
                    y = sum(c*w for c,w in zip(rgb, (0.299, 0.587, 0.114)))
                    if tone_name == "FADED":
                        # Lift the black point, compress highlights and warm the paper.
                        base = 0.12 + 0.78*y
                        color = [base + 0.54*(c-y) + bias for c,bias in
                                 zip(rgb, (0.042, 0.010, -0.030))]
                    else:
                        # Steeper midtones with a smooth shoulder and boosted chroma.
                        base = 0.35*y + 0.65*y*y*(3-2*y)
                        color = [base + 1.48*(c-y) for c in rgb]
                    table.extend(round(255*max(0,min(1,c))) for c in color)
        return bytes(table)
    rows = [line.split() for line in sources.read(tone_name + ".cube").splitlines()
            if line[:1].isdigit()]
    if tone_name == "REDSCALE":
        assert len(rows) == 13**3
        source = [tuple(map(float, row)) for row in rows]
        out = bytearray()
        for b in range(17):
            for g in range(17):
                for r in range(17):
                    coords = [v * 0.75 for v in (r,g,b)]
                    base = [min(int(v), 11) for v in coords]
                    f = [v-i for v,i in zip(coords,base)]
                    values = [0., 0., 0.]
                    for corner in range(8):
                        index, weight = 0, 1.
                        for axis in range(3):
                            upper = (corner >> axis) & 1
                            index += (base[axis]+upper) * 13**axis
                            weight *= f[axis] if upper else 1-f[axis]
                        for c in range(3):
                            values[c] += weight * source[index][c]
                    out.extend(round(max(0,min(1,v))*255) for v in values)
        return bytes(out)
    assert len(rows) == 33**3
    return bytes(round(float(v)*255) for b in range(0,33,2)
                  for g in range(0,33,2) for r in range(0,33,2)
                  for v in rows[(b*33+g)*33+r])
