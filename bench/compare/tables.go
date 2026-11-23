// bench/compare/tables.go — the work of bench/tables.nihil, in Go, for comparison: the same records
// (the same pseudo-random sequence), the same fixed-width text table, the same index on disk
// (FNV-1a of the key's text, linear probing, 4-byte slots after a 16-byte header) and the same
// syncs (File.Sync: F_FULLFSYNC on macOS, as Nihil does). It keeps its two files open, as a Go
// program would, where Nihil opens them for each operation.
//
//	go build -o tables bench/compare/tables.go && N=100000 DIR=/tmp/b ./tables
package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
)

const W = 49 // the width of a record, without its "\n"

type Item struct {
	code, category, qty int64
	name                string
	price               float64
	active              bool
}

var seed uint64 = 12345

func rnd(n int64) int64 {
	seed = (seed*1103515245 + 12345) % 2147483648
	return int64(seed / 16 % uint64(n))
}

var t0 time.Time

func start() { t0 = time.Now() }
func done(what string, count int64) {
	ms := float64(time.Since(t0).Microseconds()) / 1000
	if count > 0 {
		fmt.Printf("%s: %.1f ms (%d, %.2f µs each)\n", what, ms, count, ms*1000/float64(count))
	} else {
		fmt.Printf("%s: %.1f ms\n", what, ms)
	}
}

func checkErr(err error) { check(err) }

func check(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func floatText(v float64) string {
	s := strconv.FormatFloat(v, 'f', -1, 64) // the fewest decimals that read back the same
	if !strings.Contains(s, ".") {
		s += ".0"
	}
	return s
}

func lineOf(r *Item, b []byte) { // W bytes, no "\n"
	act := byte('F')
	if r.active {
		act = 'T'
	}
	s := fmt.Sprintf("%8d%-20s%4d%10s%6d%c", r.code, r.name, r.category, floatText(r.price), r.qty, act)
	if len(s) != W {
		fmt.Fprintln(os.Stderr, "bad width", len(s))
		os.Exit(1)
	}
	copy(b, s)
}

func intAt(s []byte, a, b int) int64 {
	for a < b && s[a] == ' ' {
		a++
	}
	neg := false
	if a < b && s[a] == '-' {
		neg = true
		a++
	}
	var v int64
	for ; a < b && s[a] >= '0' && s[a] <= '9'; a++ {
		v = v*10 + int64(s[a]-'0')
	}
	if neg {
		return -v
	}
	return v
}

func parse(s []byte, r *Item) {
	r.code = intAt(s, 0, 8)
	e := 28
	for e > 8 && s[e-1] == ' ' {
		e--
	}
	r.name = string(s[8:e])
	r.category = intAt(s, 28, 32)
	r.price, _ = strconv.ParseFloat(strings.TrimSpace(string(s[32:42])), 64)
	r.qty = intAt(s, 42, 48)
	r.active = s[48] == 'T'
}

func hash(s string) uint64 {
	h := uint64(14695981039346656037)
	for i := 0; i < len(s); i++ {
		h = (h ^ uint64(s[i])) * 1099511628211
	}
	return h
}

// CHECK=1: before each operation, a stat of the path, as Nihil does (syscall.Stat: no allocation)
var (
	checking   bool
	tdev, tino uint64
)

// is the table at the path still the file kept open? If another program replaced it, open it
// again and rebuild the index
func checkTable() {
	if !checking {
		return
	}
	var st syscall.Stat_t
	checkErr(syscall.Stat(path, &st))
	if uint64(st.Dev) == tdev && uint64(st.Ino) == tino {
		return
	}
	tf.Close()
	var err error
	tf, err = os.OpenFile(path, os.O_RDWR, 0)
	checkErr(err)
	tdev, tino = uint64(st.Dev), uint64(st.Ino)
	buildIndex()
}

var (
	path, idxPath string
	tf, xf        *os.File // the table and its index, kept open
	capacity      int64
	nrec          int64
	buf4          = make([]byte, 4)
)

func slot(j int64) uint32 {
	_, err := xf.ReadAt(buf4, 16+j*4)
	check(err)
	return binary.LittleEndian.Uint32(buf4)
}

func setSlot(j int64, v uint32, sync bool) {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, v)
	_, err := xf.WriteAt(b, 16+j*4)
	check(err)
	if sync {
		check(xf.Sync())
	}
}

func header() {
	b := make([]byte, 16)
	binary.LittleEndian.PutUint64(b, uint64(nrec*(W+1)))
	_, err := xf.WriteAt(b, 0)
	check(err)
}

func keyOfLine(i int64) int64 {
	b := make([]byte, W)
	_, err := tf.ReadAt(b, i*(W+1))
	check(err)
	return intAt(b, 0, 8)
}

// writes a whole file atomically: a temporary file, synced, renamed over the path
func writeAtomic(p string, data []byte) {
	f, err := os.Create(p + ".tmp")
	check(err)
	_, err = f.Write(data)
	check(err)
	check(f.Sync())
	check(f.Close())
	check(os.Rename(p+".tmp", p))
}

func buildIndex() {
	st, err := tf.Stat()
	check(err)
	size := st.Size()
	nrec = size / (W + 1)
	text := make([]byte, size)
	_, err = tf.ReadAt(text, 0)
	check(err)
	capacity = 16
	for capacity < 2*(nrec+1) {
		capacity *= 2
	}
	b := make([]byte, 16+capacity*4)
	binary.LittleEndian.PutUint64(b, uint64(size))
	for i := int64(0); i < nrec; i++ {
		k := strconv.FormatInt(intAt(text, int(i*(W+1)), int(i*(W+1))+8), 10)
		j := int64(hash(k) & uint64(capacity-1))
		for binary.LittleEndian.Uint32(b[16+j*4:]) != 0 {
			j = (j + 1) & (capacity - 1)
		}
		binary.LittleEndian.PutUint32(b[16+j*4:], uint32(i+1))
	}
	writeAtomic(idxPath, b)
	if xf != nil {
		xf.Close()
	}
	xf, err = os.OpenFile(idxPath, os.O_RDWR, 0)
	check(err)
}

// where key k is: its record number (or -1), and its slot (or the empty one where it would go)
func find(k int64, r *Item) (int64, int64) {
	checkTable()
	j := int64(hash(strconv.FormatInt(k, 10)) & uint64(capacity-1))
	b := make([]byte, W)
	for left := capacity; left > 0; left-- {
		v := slot(j)
		if v == 0 {
			return -1, j
		}
		_, err := tf.ReadAt(b, int64(v-1)*(W+1))
		check(err)
		if intAt(b, 0, 8) == k {
			if r != nil {
				parse(b, r)
			}
			return int64(v - 1), j
		}
		j = (j + 1) & (capacity - 1)
	}
	return -1, j
}

func put(r *Item) { // @{}>> path, r
	i, j := find(r.code, nil)
	b := make([]byte, W+1)
	lineOf(r, b)
	b[W] = '\n'
	if i >= 0 {
		_, err := tf.WriteAt(b, i*(W+1))
		check(err)
		check(tf.Sync())
	} else {
		_, err := tf.WriteAt(b, nrec*(W+1))
		check(err)
		check(tf.Sync())
		nrec++
		if nrec*2 > capacity {
			buildIndex()
			return
		}
		setSlot(j, uint32(nrec), true)
	}
	header()
}

func deleteKey(k int64) { // NAME @{k} path = _
	i, j := find(k, nil)
	if i < 0 {
		fmt.Fprintln(os.Stderr, "no key", k)
		os.Exit(1)
	}
	last, mask := nrec-1, capacity-1
	if i != last {
		b := make([]byte, W+1)
		_, err := tf.ReadAt(b, last*(W+1))
		check(err)
		moved := intAt(b, 0, 8)
		_, err = tf.WriteAt(b, i*(W+1))
		check(err)
		check(tf.Sync())
		check(tf.Truncate(last * (W + 1)))
		m := int64(hash(strconv.FormatInt(moved, 10)) & uint64(mask))
		for slot(m) != uint32(last+1) {
			m = (m + 1) & mask
		}
		setSlot(m, uint32(i+1), false)
	} else {
		check(tf.Truncate(last * (W + 1)))
		check(tf.Sync())
	}
	nrec--
	// the deleted key's slot is emptied, moving back each slot after it whose home is not between
	kk := (j + 1) & mask
	v := slot(kk)
	for v != 0 {
		h := int64(hash(strconv.FormatInt(keyOfLine(int64(v-1)), 10)) & uint64(mask))
		var stay bool
		if j < kk {
			stay = h > j && h <= kk
		} else {
			stay = h > j || h <= kk
		}
		if !stay {
			setSlot(j, v, false)
			j = kk
		}
		kk = (kk + 1) & mask
		v = slot(kk)
	}
	setSlot(j, 0, true)
	header()
}

func env(k string, d int64) int64 {
	if v, err := strconv.ParseInt(os.Getenv(k), 10, 64); err == nil {
		return v
	}
	return d
}

func main() {
	N, OPS, SYNCED := env("N", 100000), env("OPS", 20000), env("SYNCED", 300)
	checking = env("CHECK", 0) == 1
	dir := os.Getenv("DIR")
	if dir == "" {
		dir = "/tmp/nihil-bench"
	}
	os.MkdirAll(dir, 0755)
	path = dir + "/items-go.dat"
	idxPath = path + ".idx"
	os.Remove(path)
	os.Remove(idxPath)
	fmt.Printf("N = %d records of %d bytes\n", N, W+1)

	start()
	rows := make([]Item, N)
	for i := int64(0); i < N; i++ {
		p := rnd(100000)
		rows[i] = Item{code: i + 1, name: "item " + strconv.FormatInt(i+1, 10)}
		rows[i].category = rnd(50)
		rows[i].price = float64(p) / 100.0
		rows[i].qty = rnd(1000)
		rows[i].active = rnd(10) > 1
	}
	done("make the records in memory", N)

	start()
	text := make([]byte, N*(W+1))
	for i := int64(0); i < N; i++ {
		lineOf(&rows[i], text[i*(W+1):])
		text[i*(W+1)+W] = '\n'
	}
	writeAtomic(path, text)
	text = nil
	done("write the table (atomic)", N)
	st, _ := os.Stat(path)
	fmt.Printf("  file: %d KB\n", st.Size()/1024)

	start()
	all, err := os.ReadFile(path)
	check(err)
	n := int64(len(all)) / (W + 1)
	table := make([]Item, n)
	for i := int64(0); i < n; i++ {
		parse(all[i*(W+1):], &table[i])
	}
	all = nil
	done("read the whole table", n)

	start()
	found := 0
	for i := range table {
		if table[i].active && table[i].price < 200.0 {
			found++
		}
	}
	done(fmt.Sprintf("where(active && price < 200.0): %d found", found), n)

	start()
	c7 := 0
	for i := range table {
		if table[i].category == 7 {
			c7++
		}
	}
	done(fmt.Sprintf("count(category == 7): %d", c7), n)

	start()
	total := 0.0
	for i := range table {
		total += table[i].price * float64(table[i].qty)
	}
	done("sum(price * qty)", n)

	start()
	order := make([]int, n)
	for i := range order {
		order[i] = i
	}
	sort.SliceStable(order, func(a, b int) bool { return table[order[a]].price > table[order[b]].price })
	done("sort(-price)", n)

	start()
	tf, err = os.OpenFile(path, os.O_RDWR, 0)
	check(err)
	{
		var st syscall.Stat_t
		checkErr(syscall.Fstat(int(tf.Fd()), &st))
		tdev, tino = uint64(st.Dev), uint64(st.Ino)
	}
	buildIndex()
	var r Item
	find(1, &r)
	done("first key lookup (reads the table, builds the index)", 0)
	st, _ = os.Stat(idxPath)
	fmt.Printf("  index: %d KB\n", st.Size()/1024)

	start()
	for k := int64(0); k < OPS; k++ {
		if i, _ := find(rnd(N)+1, &r); i < 0 {
			fmt.Fprintln(os.Stderr, "lost")
			os.Exit(1)
		}
	}
	done("lookup by key", OPS)

	start()
	b := make([]byte, W)
	for k := int64(0); k < OPS; k++ {
		checkTable()
		_, err := tf.ReadAt(b, rnd(N)*(W+1))
		check(err)
		parse(b, &r)
	}
	done("read record i", OPS)

	start()
	for k := int64(0); k < SYNCED; k++ {
		find(rnd(N)+1, &r)
		r.qty++
		put(&r)
	}
	done("change by key (lookup + write in place, 1 fsync)", SYNCED)

	start()
	for k := int64(0); k < SYNCED; k++ {
		a := Item{code: N + 1 + k, name: "new", category: 1, price: 1.0, qty: 1, active: true}
		put(&a)
	}
	done("add by key (2 fsync)", SYNCED)

	start()
	for k := int64(0); k < SYNCED; k++ {
		deleteKey(N + 1 + k)
	}
	done("delete by key (2 fsync)", SYNCED)

	tf.Close()
	xf.Close()
	if os.Getenv("KEEP") == "" { // KEEP=1: the table stays, to compare
		os.Remove(path)
		os.Remove(idxPath)
	}
	if total < 0 {
		fmt.Println(total)
	}
}
