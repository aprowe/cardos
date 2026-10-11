"""A toy device for format 3: runs behaviour tables in a 240 px jar with
integer fixed-point physics (1/16 px), arcs to perch points and the perched
event, so the three sandbox behaviours can be seen to work -- and so the
per-item state the device would need is exactly the fields of Body below.

    python tools/bytecode_lab/btsim.py        # the three examples, 60 s
"""
import bt

STEP_HZ = 20                    # physics steps a second; item ticks are 4 a second
TICK = STEP_HZ // 4
LEFT, RIGHT = 8 * 16, 232 * 16


def xorshift(seed):
    s = [seed or 0x9E3779B9]

    def r():
        x = s[0]
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        s[0] = x
        return x
    return r


class Body:
    """Per-item runtime state. Existing JPlaced fields reused: x, vx, yoff (y),
    vy, flip, cur (frame), and the part's px/py/ptx/pty/pt for an arc. New:
    state (u8), stimer (u16), perch (u8: who and which), arc_n (u8)."""
    def __init__(self, name, table, x, y=0):
        self.name, self.t = name, bt.decode(table)
        self.x, self.y, self.vx, self.vy = x * 16, y * 16, 0, 0
        self.flip, self.cur = 0, 0
        self.state, self.stimer, self.perch = 0, 0, None
        self.ax0 = self.ay0 = self.ax1 = self.ay1 = 0      # the arc: from, to (part fields)
        self.at, self.arc_n = 0, 0
        self.home = (x * 16, y * 16)
        self.sway = 0


class Jar:
    def __init__(self, items, seed=7, log=print):
        self.items, self.rnd, self.log, self.step = items, xorshift(seed), log, 0
        for it in items:
            self.enter(it, 0, "start")

    def perches(self):
        out = []
        for k, it in enumerate(self.items):
            for j, (dx, dy) in enumerate(it.t["perches"]):
                out.append((k, j, it.home[0] + dx * 16, it.home[1] + dy * 16))
        return out

    def say(self, it, what):
        self.log("%6.2fs  %-8s x=%3d y=%2d  %s" % (self.step / STEP_HZ, it.name, it.x // 16, it.y // 16, what))

    def enter(self, it, s, why):
        it.state, it.stimer = s, 0
        st = it.t["states"][s]
        m, p = st["motion"]
        acts = [bt.dense.ACT1_TABLE[a] for a in st["enter"]]
        self.say(it, "-> state %d (%s) %s%s" % (s, why, ["still", "crawl", "roll", "arc", "perched", "sway",
                                                       "hang", "float"][m],
                                             "  does " + ", ".join("%s %d" % a for a in acts) if acts else ""))
        if m == 2:                                         # roll: an impulse along its facing
            v = p * 8 * 16 // STEP_HZ
            it.vx = -v if it.flip else v
        elif m == 3:                                       # arc: to the nest, or a random perch not its own
            ps = [q for q in self.perches() if self.items[q[0]] is not it]
            if p == 1 and ps:
                k, j, tx, ty = ps[self.rnd() % len(ps)]
                it.perch = (k, j)
            else:
                (tx, ty), it.perch = it.home, None
            pre = bt.PRESETS[it.t["preset"]]
            it.ax0, it.ay0, it.ax1, it.ay1 = it.x, it.y, tx, ty
            dist = abs(tx - it.x) // 16 + abs(ty - it.y) // 16
            it.arc_n = max(10, min(255, dist * STEP_HZ // max(1, pre[3])))
            it.at = 0
            it.flip = 1 if tx < it.x else 0
        elif m == 5:
            it.sway = p

    def fire(self, it, trig, arg=0):
        st = it.t["states"][it.state]
        for t, a, nx in st["on"]:
            if t == trig and (a == 0 or a == arg or trig in (18, 20)):
                if nx != bt.STAY:
                    self.enter(it, nx, self.trig_name(trig))
                return True
        for t, a, nx, act in it.t["globals"]:
            if t == trig:
                if act is not None:
                    self.say(it, "%s: %s %d" % (self.trig_name(trig), *bt.dense.ACT1_TABLE[act]))
                if nx != bt.STAY:
                    self.enter(it, nx, self.trig_name(trig))
                return True
        return False

    @staticmethod
    def trig_name(t):
        return {v: k for k, v in bt.TRIG.items()}[t]

    def physics(self, it):
        m, p = it.t["states"][it.state]["motion"]
        pre = bt.PRESETS[it.t["preset"]]
        if m == 2:
            it.x += it.vx
            if it.x < LEFT or it.x > RIGHT:
                it.x = LEFT if it.x < LEFT else RIGHT
                it.vx = -it.vx * pre[2] // 256
                it.flip ^= 1
                self.say(it, "bounces off the glass")
                self.fire(it, bt.TRIG["hit"])
            drag = it.vx * pre[1] // 256
            it.vx -= drag if drag else (1 if it.vx > 0 else -1 if it.vx < 0 else 0)
            it.cur = (it.x // 64) & 1                      # a rolling ball turns with the ground
            if abs(it.vx) < 4:
                it.vx = 0
                return "done"
        elif m == 3:
            it.at += 1
            t, n = it.at, it.arc_n
            mx, my = (it.ax0 + it.ax1) // 2, max(it.ay0, it.ay1) + pre[4] * 16
            # quadratic Bezier in integers: (n-t)^2 a + 2t(n-t) m + t^2 b, over n^2
            it.x = ((n - t) ** 2 * it.ax0 + 2 * t * (n - t) * mx + t * t * it.ax1) // (n * n)
            it.y = ((n - t) ** 2 * it.ay0 + 2 * t * (n - t) * my + t * t * it.ay1) // (n * n)
            if t >= n:
                if it.perch:
                    k, j = it.perch
                    self.say(it, "lands on %s's perch %d" % (self.items[k].name, j))
                    self.fire(self.items[k], bt.TRIG["perched"])
                return "done"
        elif m == 4 and p & 1 and it.stimer % TICK == 0 and self.rnd() % 12 == 0:
            it.flip ^= 1                                   # looks around
            self.flips = getattr(self, "flips", 0) + 1
        elif m == 1:
            it.x += -6 if it.flip else 6
            if it.x < LEFT or it.x > RIGHT:
                it.flip ^= 1
        return None

    def run(self, seconds, pokes=()):
        for self.step in range(int(seconds * STEP_HZ)):
            for it in self.items:
                for when, who in pokes:
                    if who == it.name and self.step == int(when * STEP_HZ):
                        self.say(it, "is poked")
                        self.fire(it, bt.TRIG["poke"])
                if self.physics(it) == "done":
                    self.fire(it, bt.TRIG["done"])
                it.stimer += 1
                if it.stimer % TICK == 0:
                    st = it.t["states"][it.state]
                    for t, a, nx in st["on"]:
                        if t == bt.TRIG["after"] and it.stimer == a * TICK:
                            self.enter(it, nx if nx != bt.STAY else it.state, "after %d" % a)
                            break
                        if t == bt.TRIG["chance"] and self.rnd() % 256 < a:
                            self.enter(it, nx if nx != bt.STAY else it.state, "chance")
                            break


if __name__ == "__main__":
    import examples
    items = [Body("pillbug", examples.BT["pillbug"], 40), Body("tree", examples.BT["tree"], 150),
             Body("bird", examples.BT["bird"], 215, 30)]
    Jar(items).run(60, pokes=[(3, "pillbug"), (40, "pillbug")])
