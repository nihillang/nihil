// bench/workload/workload.go — the work of workload.nihil in Go: N sales made up, written to a table
// (atomically, synced) and read back, added up in maps (each sale priced by one of four rules, through
// an interface), each store's best products ranked, the result written as JSON. The same bytes as the
// Nihil and C versions (run.sh compares them).
package main

import (
	"bufio"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	stores   = 20
	products = 2000
	top      = 20
)

var seed int64 = 12345

func rnd(n int64) int64 { seed = (seed*1103515245 + 12345) % 2147483648; return seed / 16 % n }

var t0 = time.Now()

func phase(name string) {
	now := time.Now()
	fmt.Fprintf(os.Stderr, "%s %d\n", name, now.Sub(t0).Milliseconds())
	t0 = now
}

func check(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
}

// a file replaced atomically: a temporary, synced, renamed (as Nihil's @>>)
func writeFile(path string, data []byte) {
	tmp := fmt.Sprintf("%s.tmp.%d", path, os.Getpid())
	f, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0644)
	check(err)
	_, err = f.Write(data)
	check(err)
	check(f.Sync())
	check(f.Close())
	check(os.Rename(tmp, path))
}

// the price rules, through an interface
type rule interface{ total(q, p int64) int64 }
type regular struct{}
type bulk struct{}
type promo struct{}
type fee struct{}

func (regular) total(q, p int64) int64 { return q * p }
func (bulk) total(q, p int64) int64 {
	if q >= 5 {
		return q * p * 90 / 100
	}
	return q * p
}
func (promo) total(q, p int64) int64 { return (q - q/3) * p }
func (fee) total(q, p int64) int64   { return q*p + q*15 }

type sale struct{ store, product, month, qty, price int64 }
type item struct{ product, revenue int64 }

// an INT field of a line: spaces, then digits
func field(s string) int64 {
	v, err := strconv.ParseInt(strings.TrimSpace(s), 10, 64)
	check(err)
	return v
}

func main() {
	n := int64(100000)
	if e := os.Getenv("N"); e != "" {
		v, err := strconv.ParseInt(e, 10, 64)
		check(err)
		n = v
	}
	dir := os.Getenv("DIR")
	if dir == "" {
		dir = "/tmp/nihil-workload"
	}
	file := dir + "/sales.dat"
	t0 = time.Now()

	// 1. generate
	var price [products + 1]int64
	var names [products + 1]string
	for p := 1; p <= products; p++ {
		price[p] = 100 + rnd(9900)
		names[p] = fmt.Sprintf("P%05d", p)
	}
	sales := make([]sale, 0, n)
	for i := int64(0); i < n; i++ {
		pr := 1 + rnd(products)
		st := 1 + rnd(stores)
		mo := 1 + rnd(12)
		q := 1 + rnd(10)
		sales = append(sales, sale{st, pr, mo, q, price[pr]})
	}
	phase("generate")

	// 2. table
	var tb strings.Builder
	w := bufio.NewWriter(&tb)
	for _, s := range sales {
		fmt.Fprintf(w, "%3d%5d%2d%2d%8d\n", s.store, s.product, s.month, s.qty, s.price)
	}
	check(w.Flush())
	writeFile(file, []byte(tb.String()))
	data, err := os.ReadFile(file)
	check(err)
	text := string(data)
	rows := make([]sale, 0, len(text)/21)
	for i := 0; i+21 <= len(text); i += 21 {
		l := text[i : i+21]
		rows = append(rows, sale{field(l[0:3]), field(l[3:8]), field(l[8:10]), field(l[10:12]), field(l[12:20])})
	}
	phase("table")

	// 3. aggregate
	rules := []rule{regular{}, bulk{}, promo{}, fee{}}
	byPair := make(map[int64]int64, 65536)
	var byMonth [13]int64
	units := make(map[string]int64, 4096)
	var grand int64
	for _, s := range rows {
		total := rules[s.product%4].total(s.qty, s.price)
		byPair[s.store*100000+s.product] += total
		byMonth[s.month] += total
		units[names[s.product]] += s.qty
		grand += total
	}
	phase("aggregate")

	// 4. rank
	tops := make([]item, 0, stores*top)
	firsts := make([]int, stores+2)
	for st := int64(1); st <= stores; st++ {
		items := make([]item, 0, products)
		for p := int64(1); p <= products; p++ {
			if v, ok := byPair[st*100000+p]; ok {
				items = append(items, item{p, v})
			}
		}
		sort.Slice(items, func(a, b int) bool {
			if items[a].revenue != items[b].revenue {
				return items[a].revenue > items[b].revenue
			}
			return items[a].product < items[b].product
		})
		firsts[st] = len(tops)
		for j := 0; j < top && j < len(items); j++ {
			tops = append(tops, items[j])
		}
	}
	firsts[stores+1] = len(tops)
	phase("rank")

	// 5. JSON
	var j strings.Builder
	j.WriteString("{\"stores\": [")
	for st := 1; st <= stores; st++ {
		if st > 1 {
			j.WriteString(", ")
		}
		j.WriteString("{\"store\": " + strconv.Itoa(st) + ", \"top\": [")
		for k := firsts[st]; k < firsts[st+1]; k++ {
			if k > firsts[st] {
				j.WriteString(", ")
			}
			it := tops[k]
			j.WriteString("{\"product\": " + strconv.FormatInt(it.product, 10) + ", \"name\": \"" + names[it.product] +
				"\", \"revenue\": " + strconv.FormatInt(it.revenue, 10) + "}")
		}
		j.WriteString("]}")
	}
	j.WriteString("], \"pairs\": [")
	npairs := 0
	for st := int64(1); st <= stores; st++ {
		for p := int64(1); p <= products; p++ {
			v, ok := byPair[st*100000+p]
			if !ok {
				continue
			}
			if npairs > 0 {
				j.WriteString(", ")
			}
			j.WriteString("{\"s\": " + strconv.FormatInt(st, 10) + ", \"p\": " + strconv.FormatInt(p, 10) +
				", \"r\": " + strconv.FormatInt(v, 10) + "}")
			npairs++
		}
	}
	j.WriteString("], \"months\": [")
	for m := 1; m <= 12; m++ {
		if m > 1 {
			j.WriteString(", ")
		}
		j.WriteString(strconv.FormatInt(byMonth[m], 10))
	}
	j.WriteString("], \"units\": {")
	for p := 1; p <= products; p++ {
		if p > 1 {
			j.WriteString(", ")
		}
		j.WriteString("\"" + names[p] + "\": " + strconv.FormatInt(units[names[p]], 10))
	}
	j.WriteString("}}\n")
	out := j.String()
	writeFile(dir+"/out.json", []byte(out))
	phase("json")

	fmt.Printf("sales %d, revenue %d, pairs %d, json %d bytes\n", n, grand, npairs, len(out))
}
