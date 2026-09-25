"""Probe: The Table's search against the REAL library JSON, mirroring the
engine algorithm (fuzzy resolution -> BM25-ish candidates -> paragraph scan).

Exercises the user's incomplete-word/typo battery:
  then friend / then friends / thn friend / then frend / then frind
and prints per-query wall time (python ~= engine's cost shape, same passes).
"""
import json, re, sys, time
from collections import defaultdict

LIB = r"C:/Users/znwaj/AppData/Local/bps/the_table.json"
ENGINE_FETCH = 40
PER_SERMON_CAP = 3
SHOW = 3

def tokens(text):
    return [t.lower() for t in re.findall(r"[0-9A-Za-z\u0080-\uffff]+", text)]

def is_word_char(c):
    return c.isalnum() or ord(c) >= 0x80

def contains_word(hay, needle):
    at = hay.find(needle)
    while at != -1:
        if (at == 0 or not is_word_char(hay[at-1])) and \
           (at+len(needle) >= len(hay) or not is_word_char(hay[at+len(needle)])):
            return True
        at = hay.find(needle, at + 1)
    return False

def starts_word(hay, needle):
    at = hay.find(needle)
    while at != -1:
        if at == 0 or not is_word_char(hay[at-1]): return True
        at = hay.find(needle, at + 1)
    return False

def contains_phrase(hay, terms):
    """Terms in order as whole words, punctuation-tolerant; the LAST term may
    end mid-word ("then friend" matches "then, friends,") — as TextMatching.hpp."""
    if not terms: return False
    if len(terms) == 1: return contains_word(hay, terms[0])
    first = terms[0]
    last = len(terms) - 1
    at = hay.find(first)
    while at != -1:
        if not (at > 0 and is_word_char(hay[at-1])):
            pos = at + len(first)
            if not (pos < len(hay) and is_word_char(hay[pos])):
                ok = True
                for ti, t in enumerate(terms[1:], 1):
                    while pos < len(hay) and not is_word_char(hay[pos]): pos += 1
                    if hay[pos:pos+len(t)] != t: ok = False; break
                    pos += len(t)
                    if ti < last and pos < len(hay) and is_word_char(hay[pos]): ok = False; break
                if ok: return True
        at = hay.find(first, at + 1)
    return False

def edit_within(a, b, max_dist):
    if a == b: return True
    if abs(len(a) - len(b)) > max_dist: return False
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j-1] + 1, prev[j-1] + (ca != cb)))
        if min(cur) > max_dist: return False
        prev = cur
    return prev[-1] <= max_dist

def is_insertion(word, w):
    """w == word + one inserted letter (the dropped-letter typo shape)."""
    if len(w) != len(word) + 1: return False
    i = j = 0; skipped = False
    while i < len(word) and j < len(w):
        if word[i] == w[j]: i += 1; j += 1; continue
        if skipped: return False
        skipped = True; j += 1
    return True

def resolve(word, vocab_df):
    """word in vocab -> None (as typed); else completion or 1-2-edit correction.
    Equal-distance ties prefer the insertion shape (thn->then over thn->the),
    then frequency — mirroring TextMatching.hpp."""
    if word in vocab_df: return None
    # Jam-split: two words typed without the space ("holyspirit") — the pair of
    # real words that reassembles it, best corpus coverage (min df) wins.
    if len(word) >= 6:
        best, bdf = None, 0
        for cut in range(3, len(word) - 2):
            a, b = word[:cut], word[cut:]
            if a in vocab_df and b in vocab_df:
                m = min(vocab_df[a], vocab_df[b])
                if m > bdf: best, bdf = a + " " + b, m
        if best: return best
    cands = [(df, w) for w, df in vocab_df.items() if w.startswith(word)]
    if cands:
        cands.sort(reverse=True)
        return cands[0][1]
    max_d = 1 if len(word) <= 4 else 2
    best, bd, bdf, bins = "", max_d + 1, 0, False
    for w, df in vocab_df.items():
        if len(w) < 3 or w[0] != word[0]: continue
        if abs(len(w) - len(word)) > max_d: continue
        d = next((dd for dd in range(1, max_d + 1) if edit_within(word, w, dd)), None)
        if d is None: continue
        ins = is_insertion(word, w)
        better = d < bd or (d == bd and ((ins and not bins) or (ins == bins and df > bdf)))
        if better:
            best, bd, bdf, bins = w, d, df, ins
    return best or None

print(f"loading {LIB} ...")
t0 = time.perf_counter()
with open(LIB, encoding="utf-8") as f:
    data = json.load(f)
chapters = {}
for book in data.get("books", []):
    for ch in book.get("chapters", []):
        did = f"table:{book['id']}:{ch['number']}"
        chapters[did] = [v.get("text", "").lower() for v in ch.get("verses", [])]
order = sorted(chapters)
doc_tokens = {did: set(tokens(did + " " + " ".join(chapters[did]))) for did in order}
vocab_df = defaultdict(int)
for did in order:
    for t in doc_tokens[did]:
        vocab_df[t] += 1
print(f"  {len(order)} chapters, {len(vocab_df)} vocabulary words  "
      f"(load+index {time.perf_counter()-t0:.1f}s)")

QUERIES = sys.argv[1:] or ["then friend", "then friends", "thn friend",
                           "then frend", "then frind", "then, friends"]
for QUERY in QUERIES:
    tq = time.perf_counter()
    raw = []
    for t in tokens(QUERY):
        if len(t) >= 2 and t not in raw: raw.append(t)
    resolved = [(t, resolve(t, vocab_df)) for t in raw]
    # split jam results ("holy spirit") into real terms, as the C++ does
    terms = []
    for t, r in resolved:
        terms.extend((r or t).split())
    boni = [t for t, r in resolved if r and r.split() != [t]]
    # Candidates mirror the engine: every term whole, OR the missing term via
    # its top-df prefix siblings ("friend" inside "friends") — those docs are
    # IN, with partial credit, no longer dropped before the scan.
    cand = []
    for did in order:
        ok = True
        for t in terms:
            if t in doc_tokens[did]: continue
            if any(w.startswith(t) and w in doc_tokens[did] for w in vocab_df): continue
            ok = False; break
        if ok: cand.append(did)
    cand = cand[:ENGINE_FETCH]
    hits = []
    for rank, did in enumerate(cand):
        paras = chapters[did]
        n = len(paras)
        bm = [0]*n; wm = [0]*n; bx = [0]*n
        for i, p in enumerate(paras):
            for k, t in enumerate(terms):
                if t in p:
                    bm[i] |= 1 << k
                    # whole word OR word-start extension ("friend" in "friends")
                    # = full strength; mid-word substring = weak (no w bit)
                    if contains_word(p, t) or starts_word(p, t): wm[i] |= 1 << k
            for k, b in enumerate(boni[:32]):
                if contains_word(p, b): bx[i] |= 1 << k
        all_m = (1 << len(terms)) - 1
        def quality(m, w):
            return sum(((m >> k) & 1) + ((w >> k) & 1) for k in range(min(len(terms),32)))
        def bonus_ct(m):
            return bin(m).count("1")
        for i in range(n):
            if bm[i] == all_m:
                sc = (1000 + quality(bm[i], wm[i])
                      + (500 if contains_phrase(paras[i], terms) else 0)
                      + 20 * bonus_ct(bx[i]))
                hits.append([sc, False, did, i+1, rank])
        for i in range(n-1):
            if (bm[i] | bm[i+1]) == all_m and bm[i] != all_m and bm[i+1] != all_m:
                joined = paras[i] + " " + paras[i+1]
                sc = (quality(bm[i]|bm[i+1], wm[i]|wm[i+1])
                      + (500 if contains_phrase(joined, terms) else 0)
                      + 20 * bonus_ct(bx[i] | bx[i+1]))
                hits.append([sc, True, did, i+1, rank])
    # Sermon-level engine rank now mirrors BM25 + prefix credit + proximity +
    # phrase: score each candidate doc like the engine before assigning ranks.
    def engine_score(did):
        content = " ".join(chapters[did])
        s = 0.0
        for t in terms:
            if t in doc_tokens[did]:
                df = vocab_df[t]
                idf = __import__("math").log(1 + (len(order) - df + 0.5) / (df + 0.5))
                tf = content.count(t)
                dl = max(1, len(content) // 6)
                avg = sum(len(" ".join(chapters[d])) // 6 for d in order) / len(order)
                s += idf * (tf * 2.2) / (tf + 1.2 * (1 - 0.75 + 0.75 * dl / avg))
            elif any(w in doc_tokens[did] for w in vocab_df if w.startswith(t)):
                s += 1.5   # partial credit, flat-ish like the 40% branch
        if len(terms) > 1 and contains_phrase(content, terms): s += 20
        elif len(terms) > 1:
            at = [content.find(t) for t in terms]
            if all(p >= 0 for p in at):
                spread = (max(at) - min(at)) / 240.0
                if spread <= 1.0: s += 8.0 * (1.0 - spread)
        return s
    ranks = sorted(cand, key=engine_score, reverse=True)
    rank_of = {d: i for i, d in enumerate(ranks)}
    for h in hits:
        h[4] = rank_of.get(h[2], h[4])
    hits.sort(key=lambda h: (h[1], -h[0], h[4], h[3]))
    kept, per = [], defaultdict(int)
    for h in hits:
        if per[h[2]] >= PER_SERMON_CAP: continue
        per[h[2]] += 1
        kept.append(h)
    ms = (time.perf_counter() - tq) * 1000
    print(f"\n=== {QUERY!r}  ->  looks for {terms}" + (f"  (+bonus {boni})" if boni else "")
          + f"\n    candidates={len(cand)} hits={len(hits)} shown={min(SHOW,len(kept))}  [{ms:.0f} ms]")
    for h in kept[:SHOW]:
        print(f"    score={h[0]} spanned={h[1]} {h[2]} para {h[3]}: {chapters[h[2]][h[3]-1][:58]}")
    if "then" in terms or "friend" in terms:   # the para-7 regression check only fits that query
        ok = any(h[2] == "table:Y1947:1" and h[3] == 7 for h in kept[:SHOW])
        print(f"    para 7 of 47-0412 in the top {SHOW}? {ok}")
