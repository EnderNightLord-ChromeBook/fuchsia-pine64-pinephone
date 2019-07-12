// Copyright 2017 The Netstack Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package dns

import (
	"fmt"
	"math"
	"sync"
	"time"

	"syslog"

	"golang.org/x/net/dns/dnsmessage"
)

const (
	// TODO: Think about a good value. dnsmasq defaults to 150 names.
	maxEntries = 1024
	tag        = "DNS"
)

var testHookNow = func() time.Time { return time.Now() }

// Single entry in the cache, like a TypeA resource holding an IPv4 address.
type cacheEntry struct {
	rr  dnsmessage.Resource // the resource
	ttd time.Time           // when this entry expires
}

// Returns true if this entry is a CNAME that points at something no longer in our cache.
func (entry *cacheEntry) isDanglingCNAME(cache *cacheInfo) bool {
	switch rr := entry.rr.Body.(type) {
	case *dnsmessage.CNAMEResource:
		return cache.m[rr.CNAME] == nil
	default:
		return false
	}
}

// The full cache.
type cacheInfo struct {
	mu         sync.Mutex
	m          map[dnsmessage.Name][]cacheEntry
	numEntries int
}

func newCache() cacheInfo {
	return cacheInfo{m: make(map[dnsmessage.Name][]cacheEntry)}
}

// Returns a list of Resources that match the given Question (same class and type and matching domain name).
func (cache *cacheInfo) lookup(question dnsmessage.Question) []dnsmessage.Resource {
	entries := cache.m[question.Name]

	rrs := make([]dnsmessage.Resource, 0, len(entries))
	for _, entry := range entries {
		h := entry.rr.Header
		if h.Class == question.Class && h.Name == question.Name {
			switch body := entry.rr.Body.(type) {
			case *dnsmessage.CNAMEResource:
				rrs = append(rrs, cache.lookup(dnsmessage.Question{
					Name:  body.CNAME,
					Class: question.Class,
					Type:  question.Type,
				})...)
			default:
				if h.Type == question.Type {
					rrs = append(rrs, entry.rr)
				}
			}
		}
	}
	return rrs
}

// Finds the minimum TTL value of any SOA resource in a response. Returns 0 if not found.
// This is used for caching a failed DNS query. See RFC 2308.
func findSOAMinTTL(auths []dnsmessage.Resource) uint32 {
	minTTL := uint32(math.MaxUint32)
	foundSOA := false
	for _, auth := range auths {
		if auth.Header.Class == dnsmessage.ClassINET {
			switch soa := auth.Body.(type) {
			case *dnsmessage.SOAResource:
				foundSOA = true
				if soa.MinTTL < minTTL {
					minTTL = soa.MinTTL
				}
			}
		}
	}
	if foundSOA {
		return minTTL
	}
	return 0
}

// Attempts to add a new entry into the cache. Can fail if the cache is full.
func (cache *cacheInfo) insert(rr dnsmessage.Resource) {
	h := rr.Header
	newEntry := cacheEntry{
		ttd: testHookNow().Add(time.Duration(h.TTL) * time.Second),
		rr:  rr,
	}

	entries := cache.m[h.Name]
	for i := range entries {
		existing := &entries[i]
		if h.Class != existing.rr.Header.Class || h.Type != existing.rr.Header.Type || h.Name != existing.rr.Header.Name {
			continue
		}
		if existing.rr.Body == nil {
			// We have a valid record now; replace the negative resource entirely.
			existing.rr = rr
			existing.ttd = newEntry.ttd
		} else {
			switch b1 := rr.Body.(type) {
			case *dnsmessage.AResource:
				if b2, ok := existing.rr.Body.(*dnsmessage.AResource); !ok || b1.A != b2.A {
					continue
				}
			case *dnsmessage.AAAAResource:
				if b2, ok := existing.rr.Body.(*dnsmessage.AAAAResource); !ok || b1.AAAA != b2.AAAA {
					continue
				}
			case *dnsmessage.CNAMEResource:
				if b2, ok := existing.rr.Body.(*dnsmessage.CNAMEResource); !ok || b1.CNAME != b2.CNAME {
					continue
				}
			default:
				panic(fmt.Sprintf("unknown type %T", b1))
			}
			if newEntry.ttd.After(existing.ttd) {
				existing.ttd = newEntry.ttd
			}
		}
		syslog.VLogTf(syslog.TraceVerbosity, tag, "DNS cache update: %v(%v) expires %v", h.Name, h.Type, existing.ttd)
		return
	}
	if cache.numEntries+1 <= maxEntries {
		syslog.VLogTf(syslog.TraceVerbosity, tag, "DNS cache insert: %v(%v) expires %v", h.Name, h.Type, newEntry.ttd)
		cache.m[h.Name] = append(entries, newEntry)
		cache.numEntries++
	} else {
		// TODO(mpcomplete): might be better to evict the LRU entry instead.
		// TODO(mpcomplete): RFC 1035 7.4 says that if we can't cache this RR, we
		// shouldn't cache any other RRs for the same name in this response.
		syslog.WarnTf(tag, "DNS cache is full; insert failed: %v(%v)", h.Name, h.Type)
	}
}

// Attempts to add each Resource as a new entry in the cache. Can fail if the cache is full.
func (cache *cacheInfo) insertAll(rrs []dnsmessage.Resource) {
	cache.prune()
	for _, rr := range rrs {
		h := rr.Header
		if h.Class == dnsmessage.ClassINET {
			switch h.Type {
			case dnsmessage.TypeA, dnsmessage.TypeAAAA, dnsmessage.TypeCNAME:
				cache.insert(rr)
			}
		}
	}
}

func (cache *cacheInfo) insertNegative(question dnsmessage.Question, msg dnsmessage.Message) {
	cache.prune()
	minTTL := findSOAMinTTL(msg.Authorities)
	if minTTL == 0 {
		// Don't cache without a TTL value.
		return
	}
	cache.insert(dnsmessage.Resource{
		Header: dnsmessage.ResourceHeader{
			Name:  question.Name,
			Type:  question.Type,
			Class: dnsmessage.ClassINET,
			TTL:   minTTL,
		},
		Body: nil,
	})
}

// Removes every expired/dangling entry from the cache.
func (cache *cacheInfo) prune() {
	now := testHookNow()
	for name, entries := range cache.m {
		removed := false
		for i := 0; i < len(entries); {
			if now.After(entries[i].ttd) || entries[i].isDanglingCNAME(cache) {
				entries[i] = entries[len(entries)-1]
				entries = entries[:len(entries)-1]
				cache.numEntries--
				removed = true
			} else {
				i++
			}
		}
		if len(entries) == 0 {
			delete(cache.m, name)
		} else if removed {
			cache.m[name] = entries
		}
	}
}

var cache = newCache()

func newCachedResolver(fallback Resolver) Resolver {
	var mu struct {
		sync.Mutex
		throttled map[dnsmessage.Question]struct{}
	}
	mu.throttled = make(map[dnsmessage.Question]struct{})
	return func(c *Client, question dnsmessage.Question) (dnsmessage.Name, []dnsmessage.Resource, dnsmessage.Message, error) {
		if !(question.Class == dnsmessage.ClassINET && (question.Type == dnsmessage.TypeA || question.Type == dnsmessage.TypeAAAA)) {
			panic("unexpected question type")
		}

		cache.mu.Lock()
		rrs := cache.lookup(question)
		cache.mu.Unlock()
		if len(rrs) != 0 {
			syslog.VLogTf(syslog.TraceVerbosity, tag, "DNS cache hit %v(%v) => %v", question.Name, question.Type, rrs)
			return dnsmessage.Name{}, rrs, dnsmessage.Message{}, nil
		}

		syslog.VLogTf(syslog.TraceVerbosity, tag, "DNS cache miss for the message %v(%v)", question.Name, question.Type)
		cname, rrs, msg, err := fallback(c, question)
		if err != nil {
			mu.Lock()
			_, throttled := mu.throttled[question]
			if !throttled {
				mu.throttled[question] = struct{}{}
			}
			mu.Unlock()
			if !throttled {
				time.AfterFunc(10*time.Second, func() {
					mu.Lock()
					delete(mu.throttled, question)
					mu.Unlock()
				})

				syslog.VLogTf(syslog.DebugVerbosity, tag, "%s", err)
			}
		}

		cache.mu.Lock()
		if err == nil {
			cache.insertAll(msg.Answers)
		} else if err, ok := err.(*Error); ok && err.CacheNegative {
			cache.insertNegative(question, msg)
		}
		cache.mu.Unlock()

		return cname, rrs, msg, err
	}
}
