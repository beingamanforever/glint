// glint: trigram-indexed grep for coding agents.
//   glint index [DIR]
//   glint [-i] [-F] [-l] [-c] [-m N] [--labels TSV] [--stats] [--no-index] [--no-fresh] PATTERN [DIR]
// ----------------------------------------------------------------------------
// Observations
// 1. Reading files dominates; trigrams pick the few worth reading (Russ Cox).
// 2. Sorted lookup table + delta-varint postings, mmap'd, binary-searched (Cursor, tgrep).
// 3. ASCII-folded trigrams serve exact and -i search (tgrep).
// 4. Plans are conservative: syntax the planner can't read scans every file (tgrep).
// 5. PCRE2 JIT verifies whole buffers in parallel (ripgrep).
// 6. Size/mtime stamps keep results fresh without calling git per query (tgrep).
// 7. -m caps output for agents; --labels tags hits, e.g. with PRISM node ids (codedb).
// ----------------------------------------------------------------------------
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace std;

#define all(x) (x).begin(), (x).end()
#define pb push_back
typedef uint32_t u32;
typedef uint64_t u64;
typedef vector<u32> vu;
typedef vector<string> vs;
typedef vector<vs> Plan;  // OR of ANDs of required literals

#ifdef __APPLE__
#define MTIME(st) ((st).st_mtimespec.tv_sec * 1000000000LL + (st).st_mtimespec.tv_nsec)
#else
#define MTIME(st) ((st).st_mtim.tv_sec * 1000000000LL + (st).st_mtim.tv_nsec)
#endif

struct Entry { u32 tri, count; u64 offset; };  // lookup.bin row
struct Stamp { int64_t size, mtime; };         // per file, then .git/index
const size_t MAX_FILE = 16 << 20;
const unsigned THREADS = max(1u, thread::hardware_concurrency());

double ms_since(chrono::steady_clock::time_point t)
{
    return chrono::duration<double, milli>(chrono::steady_clock::now() - t).count();
}

unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
string index_dir(const string& dir) { return dir + "/.git/glint"; }

template <class F> void parallel(size_t n, F work)
{
    atomic<size_t> next{0};
    vector<thread> pool;
    for (unsigned t = 0; t < THREADS; t++) pool.emplace_back([&] { for (size_t k; (k = next++) < n;) work(k); });
    for (auto& th : pool) th.join();
}

vs split0(const string& s)
{
    vs out;
    for (size_t i = 0, j; i < s.size(); i = j + 1)
    {
        if ((j = s.find('\0', i)) == string::npos) j = s.size();
        if (j > i) out.pb(s.substr(i, j - i));
    }
    return out;
}

vs list_files(const string& dir)
{
    string cmd = "git -C '", out;
    for (char c : dir) cmd += c == '\'' ? "'\\''" : string(1, c);
    char buf[1 << 16];
    if (FILE* p = popen((cmd + "' ls-files -z 2>/dev/null").c_str(), "r"))
    {
        for (size_t n; (n = fread(buf, 1, sizeof buf, p));) out.append(buf, n);
        pclose(p);
    }
    return split0(out);
}

// false for binary files: a NUL in the first 8 KiB
bool read_text(const string& path, string& buf, Stamp* stamp = nullptr)
{
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (size_t)st.st_size <= MAX_FILE;
    if (ok)
    {
        buf.resize(st.st_size);
        size_t got = 0;
        for (ssize_t n; got < buf.size() && (n = read(fd, &buf[got], buf.size() - got)) > 0;) got += n;
        buf.resize(got);
        if (stamp) *stamp = {st.st_size, MTIME(st)};
    }
    close(fd);
    return ok && !memchr(buf.data(), 0, min<size_t>(buf.size(), 8192));
}

int build(const string& dir)
{
    auto t0 = chrono::steady_clock::now();
    vs files = list_files(dir), kept, lists;
    if (files.empty()) return fprintf(stderr, "glint: %s is not a git repository with tracked files\n", dir.c_str()), 2;
    vu slot(1 << 24, UINT32_MAX), last, tris, counts;  // trigram -> list id
    vector<Stamp> stamps;
    const size_t WINDOW = 4096;  // extract in parallel, append in file order
    vector<vu> grams(WINDOW);
    vector<Stamp> st(WINDOW);
    vector<char> ok(WINDOW);
    for (size_t base = 0; base < files.size(); base += WINDOW)
    {
        size_t n = min(WINDOW, files.size() - base);
        parallel(n, [&](size_t k)
        {
            thread_local string buf;
            thread_local vector<u64> seen(1 << 18);  // 2^24-bit set
            vu& g = grams[k];
            g.clear();
            if (!(ok[k] = read_text(dir + "/" + files[base + k], buf, &st[k]))) return;
            u32 tri = 0;
            for (size_t i = 0; i < buf.size(); i++)
            {
                tri = (tri << 8 | lower(buf[i])) & 0xFFFFFF;
                if (i >= 2 && !(seen[tri >> 6] >> (tri & 63) & 1)) seen[tri >> 6] |= 1ULL << (tri & 63), g.pb(tri);
            }
            for (u32 x : g) seen[x >> 6] = 0;
        });
        for (size_t k = 0; k < n; k++)
        {
            if (!ok[k]) continue;
            u32 id = kept.size();
            kept.pb(files[base + k]), stamps.pb(st[k]);
            for (u32 tri : grams[k])
            {
                if (slot[tri] == UINT32_MAX) slot[tri] = lists.size(), lists.pb(""), last.pb(0), tris.pb(tri), counts.pb(0);
                u32 s = slot[tri], d = id - last[s];
                for (last[s] = id, counts[s]++; d >= 0x80; d >>= 7) lists[s] += char(d | 0x80);
                lists[s] += char(d);
            }
        }
    }
    struct stat gi;
    if (stat((dir + "/.git/index").c_str(), &gi) == 0) stamps.pb({gi.st_size, MTIME(gi)});
    string out = index_dir(dir);
    mkdir(out.c_str(), 0755);
    vu order(tris.size());
    iota(all(order), 0);
    sort(all(order), [&](u32 a, u32 b) { return tris[a] < tris[b]; });
    ofstream post(out + "/postings.bin", ios::binary), table(out + "/lookup.bin", ios::binary);
    u64 offset = 0, postings = 0;
    for (u32 s : order)
    {
        Entry e{tris[s], counts[s], offset};
        table.write((const char*)&e, sizeof e), post << lists[s];
        offset += lists[s].size(), postings += counts[s];
    }
    ofstream names(out + "/files.bin", ios::binary);
    for (auto& f : kept) names << f << '\0';
    ofstream(out + "/stamps.bin", ios::binary).write((const char*)stamps.data(), stamps.size() * sizeof(Stamp));
    printf("indexed %zu files, %zu trigrams, %llu postings, %.1f MB in %.0f ms\n", kept.size(), tris.size(),
           (unsigned long long)postings, (offset + tris.size() * sizeof(Entry)) / 1e6, ms_since(t0));
    return 0;
}

struct Mapped
{
    const unsigned char* data = nullptr;
    size_t size = 0;
    explicit Mapped(const string& path)
    {
        int fd = open(path.c_str(), O_RDONLY);
        struct stat st;
        if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0)
            data = (const unsigned char*)mmap(nullptr, size = st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (fd >= 0) close(fd);
        if (data == MAP_FAILED) data = nullptr, size = 0;
    }
};

struct Index
{
    Mapped table, post, names, stamps;
    vs files;
    int lookups = 0;
    explicit Index(const string& d)
        : table(d + "/lookup.bin"), post(d + "/postings.bin"), names(d + "/files.bin"), stamps(d + "/stamps.bin")
    {
        files = split0(string((const char*)names.data, names.size));
    }
    bool ok() const { return table.data && post.data && stamps.size == (files.size() + 1) * sizeof(Stamp); }
    vu posting(u32 tri)
    {
        lookups++;
        auto* begin = (const Entry*)table.data;
        auto* end = begin + table.size / sizeof(Entry);
        auto* e = lower_bound(begin, end, tri, [](const Entry& x, u32 t) { return x.tri < t; });
        if (e == end || e->tri != tri) return {};
        vu ids(e->count);
        const unsigned char* p = post.data + e->offset;
        u32 id = 0;
        for (u32& out : ids)
        {
            u32 d = 0;
            for (int shift = 0;; shift += 7) { d |= u32(*p & 0x7F) << shift; if (!(*p++ & 0x80)) break; }
            out = id += d;
        }
        return ids;
    }
};

// Required literals per alternative; empty means scan every file.
Plan plan(const string& p, bool fixed)
{
    if (fixed) return {{p}};
    Plan alts(1);
    string cur;
    int depth = 0;
    bool cls = false;
    auto flush = [&] { if (cur.size() >= 3) alts.back().pb(cur); cur.clear(); };
    for (size_t i = 0; i < p.size(); i++)
    {
        char c = p[i];
        if (c == '\\' && i + 1 < p.size())
        {
            char e = p[++i];
            if (cls || depth) continue;
            if (!isalnum((unsigned char)e)) cur += e;
            else if (strchr("wWdDsSbB", e)) flush();
            else return {};  // \x41, \1, \p{..}
        }
        else if (cls) cls = c != ']';
        else if (depth) depth += (c == '(') - (c == ')');
        else if (c == '|') flush(), alts.pb({});
        else if (c == '*' || c == '?' || c == '{')  // previous char optional
        {
            if (!cur.empty()) cur.pop_back();
            flush();
            if (c == '{') i = min(p.find('}', i), p.size());
        }
        else if (strchr("+.^$[(", c)) flush(), cls = c == '[', depth = c == '(';
        else cur += c;
    }
    flush();
    for (auto& a : alts) if (a.empty()) return {};
    return alts;
}

bool candidates(Index& ix, const Plan& pl, bool ascii_only, vu& out)
{
    for (auto& alt : pl)
    {
        vu tris;
        for (auto& lit : alt)
            for (size_t i = 0; i + 3 <= lit.size(); i++)
            {
                u32 t = lower(lit[i]) << 16 | lower(lit[i + 1]) << 8 | lower(lit[i + 2]);
                if (!ascii_only || !(t & 0x808080)) tris.pb(t);
            }
        if (tris.empty()) return false;
        sort(all(tris));
        tris.erase(unique(all(tris)), tris.end());
        vector<vu> lists;
        for (u32 t : tris) lists.pb(ix.posting(t));
        sort(all(lists), [](const vu& a, const vu& b) { return a.size() < b.size(); });
        vu hit = lists[0], tmp;
        for (size_t k = 1; k < lists.size() && !hit.empty(); k++)
        {
            tmp.clear();
            set_intersection(all(hit), all(lists[k]), back_inserter(tmp));
            hit.swap(tmp);
        }
        out.insert(out.end(), all(hit));
    }
    sort(all(out));
    out.erase(unique(all(out)), out.end());
    return true;
}

struct Options { bool icase = false, fixed = false, files_only = false, count = false, stats = false, no_index = false, no_fresh = false; size_t max = 0; string labels; };

int search(const string& pattern, const string& dir, const Options& o)
{
    auto t0 = chrono::steady_clock::now();
    int err;
    PCRE2_SIZE at;
    u32 flags = (o.fixed ? PCRE2_LITERAL : PCRE2_MULTILINE) | (o.icase ? PCRE2_CASELESS : 0);
    pcre2_code* re = pcre2_compile((PCRE2_SPTR)pattern.c_str(), pattern.size(), flags, &err, &at, nullptr);
    if (!re)
    {
        PCRE2_UCHAR msg[256];
        pcre2_get_error_message(err, msg, sizeof msg);
        return fprintf(stderr, "glint: bad pattern at %zu: %s\n", (size_t)at, (const char*)msg), 2;
    }
    pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);

    Index ix(index_dir(dir));
    bool indexed = !o.no_index && ix.ok(), fresh = indexed && !o.no_fresh;
    const Stamp* stamp = (const Stamp*)ix.stamps.data;
    size_t n = indexed ? ix.files.size() : 0;
    vs listed;
    struct stat gi;
    if (!indexed) listed = list_files(dir);
    else if (fresh && stat((dir + "/.git/index").c_str(), &gi) == 0 && MTIME(gi) != stamp[n].mtime)
    {
        listed = ix.files;  // files added to git since indexing go last
        unordered_set<string> known(all(ix.files));
        for (auto& f : list_files(dir)) if (!known.count(f)) listed.pb(f);
    }
    const vs& files = listed.empty() ? ix.files : listed;
    Plan pl = plan(pattern, o.fixed);
    vu cand;
    if (!indexed || pl.empty() || !candidates(ix, pl, o.icase || pattern.find("(?") != string::npos, cand))
    {
        cand.resize(files.size());
        iota(all(cand), 0);
    }
    vector<char> changed(fresh ? files.size() : 0, 1);
    parallel(fresh ? n : 0, [&](size_t k)
    {
        struct stat st;
        changed[k] = stat((dir + "/" + files[k]).c_str(), &st) == 0 && (st.st_size != stamp[k].size || MTIME(st) != stamp[k].mtime);
    });
    for (u32 k = 0; k < changed.size(); k++) if (changed[k]) cand.pb(k);
    sort(all(cand));
    cand.erase(unique(all(cand)), cand.end());
    double plan_ms = ms_since(t0);

    struct Hit { string out; size_t lines = 0; };
    vector<Hit> hits(cand.size());
    parallel(cand.size(), [&](size_t k)
    {
        thread_local string buf;
        thread_local unique_ptr<pcre2_match_data, void (*)(pcre2_match_data*)> md(
            pcre2_match_data_create_from_pattern(re, nullptr), pcre2_match_data_free);
        const string& path = files[cand[k]];
        if (!read_text(dir + "/" + path, buf)) return;
        Hit& h = hits[k];
        for (size_t pos = 0, line = 1, counted = 0;
             pos < buf.size() && pcre2_match(re, (PCRE2_SPTR)buf.data(), buf.size(), pos, 0, md.get(), nullptr) >= 0;)
        {
            size_t bol = pcre2_get_ovector_pointer(md.get())[0], eol = buf.find('\n', bol);
            while (bol > pos && buf[bol - 1] != '\n') bol--;
            if (eol == string::npos) eol = buf.size();
            line += count(buf.begin() + counted, buf.begin() + bol, '\n');
            counted = bol, h.lines++, pos = eol + 1;
            if (o.files_only) break;
            if (!o.count) h.out.append(path).append(":").append(to_string(line)).append(":").append(buf, bol, eol - bol) += '\n';
        }
    });

    unordered_map<string, string> tags;  // path -> labels
    ifstream tsv(o.labels);
    for (string line; getline(tsv, line);)
        if (size_t t = line.find('\t'); t != string::npos)
        {
            string& s = tags[line.substr(0, t)];
            s += (s.empty() ? "" : ",") + line.substr(t + 1);
        }

    size_t total = 0, shown = 0, matched = 0;
    string out;
    for (size_t k = 0; k < cand.size(); k++)
    {
        const Hit& h = hits[k];
        if (!h.lines) continue;
        matched++, total += h.lines;
        const string& path = files[cand[k]];
        auto it = tags.find(path);
        string tag = it == tags.end() ? "" : "\t[" + it->second + "]";
        if (o.files_only) out += path + tag + '\n';
        else if (o.count) out += path + ':' + to_string(h.lines) + tag + '\n';
        else
            for (size_t i = 0, j; i < h.out.size() && (!o.max || shown < o.max); i = j + 1, shown++)
                j = h.out.find('\n', i), out.append(h.out, i, j - i).append(tag) += '\n';
    }
    if (o.max && total > shown)
        out += "glint: showed " + to_string(shown) + " of " + to_string(total) + " matching lines in " +
               to_string(matched) + " files; narrow the pattern or add a path\n";
    fwrite(out.data(), 1, out.size(), stdout);
    if (o.stats)
        fprintf(stderr, "{\"indexed\":%s,\"lookups\":%d,\"changed\":%zu,\"candidates\":%zu,\"files\":%zu,\"matched_files\":%zu,"
                "\"lines\":%zu,\"plan_ms\":%.2f,\"total_ms\":%.2f,\"threads\":%u}\n", indexed ? "true" : "false", ix.lookups,
                (size_t)count(all(changed), 1), cand.size(), files.size(), matched, total, plan_ms, ms_since(t0), THREADS);
    pcre2_code_free(re);
    return total ? 0 : 1;
}

int main(int argc, char** argv)
{
    vs args(argv + 1, argv + argc), pos;
    if (!args.empty() && args[0] == "index") return build(args.size() > 1 ? args[1] : ".");
    Options o;
    for (size_t i = 0; i < args.size(); i++)
    {
        const string& a = args[i];
        if (a == "-i") o.icase = true;
        else if (a == "-F") o.fixed = true;
        else if (a == "-l") o.files_only = true;
        else if (a == "-c") o.count = true;
        else if (a == "-m" && i + 1 < args.size()) o.max = stoul(args[++i]);
        else if (a == "--labels" && i + 1 < args.size()) o.labels = args[++i];
        else if (a == "--stats") o.stats = true;
        else if (a == "--no-index") o.no_index = true;
        else if (a == "--no-fresh") o.no_fresh = true;
        else if (a == "-e" && i + 1 < args.size()) pos.pb(args[++i]);
        else pos.pb(a);
    }
    if (pos.empty() || pos.size() > 2)
        return fprintf(stderr, "usage: glint index [DIR]\n       glint [-i] [-F] [-l] [-c] [-m N] [--labels TSV] [--stats] [--no-index] [--no-fresh] PATTERN [DIR]\n"), 2;
    return search(pos[0], pos.size() > 1 ? pos[1] : ".", o);
}
