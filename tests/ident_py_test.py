# fm1_install.py's handshake against M-UPGRADE 3.11's short identities (the same cases as tests/ident_test.mjs).
import sys
sys.path.insert(0, 'base/tools')
import fm1_install as I
def ident_frame(txt):
    t = txt.encode()
    return b"\xF0" + I.pack7(bytes([0, 0x59, 0x11, 27, 0, 0]) + t + bytes(28 - len(t))) + b"\xF7"
class FakeLink:
    def __init__(self, plain, full):
        self.plain, self.full, self.q, self.nfull = plain, full, [], 0
    def drain(self): self.q.clear()
    def send(self, b):
        if bytes(b) == I.HS_QUERY: self.q.append(ident_frame(self.plain))
        elif bytes(b) == I.HS_FULL:
            self.nfull += 1
            if self.full: self.q.append(ident_frame(self.full))
        return True
    def read(self, t): return self.q.pop(0) if self.q else None
bad = 0
def ok(what, c):
    global bad
    print(("ok   " if c else "FAIL ") + what); bad += not c
I.DELAY["hs"] = 0.05
l = FakeLink("FM-1_606", "FM-1_606010000"); r = I.handshake(l)
ok("a PATCHWERK: the whole identity from the installers' query", r.text == "FM-1_606010000" and r.model == "FM-1" and l.nfull == 1)
l = FakeLink("FM-1_606010000", None); r = I.handshake(l)
ok("an older PATCHWERK: taken as it is, no second query", r.text == "FM-1_606010000" and l.nfull == 0)
l = FakeLink("FM-1_606", None); r = I.handshake(l)
ok("no answer to the second query: the short identity stands", r.text == "FM-1_606" and l.nfull == 1)
l = FakeLink("FM-1_015", "FM-1_015999"); r = I.handshake(l)
ok("M-VAVE's stock FM-1: never asked again", r.text == "FM-1_015" and l.nfull == 0)
print("IDENT PY FAILED" if bad else "IDENT PY PASSED")
sys.exit(1 if bad else 0)
