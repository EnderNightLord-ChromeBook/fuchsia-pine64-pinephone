// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package daemon

import (
	"fmt"
	"io/ioutil"
	"log"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall/zx"

	"fidl/fuchsia/amber"
	"fidl/fuchsia/pkg"

	"amber/atonce"
	"amber/metrics"
	"amber/source"
)

const (
	PackageGarbageDir = "/pkgfs/ctl/garbage"
)

type Daemon struct {
	store string
	pkgfs source.PkgfsDir

	muSrcs sync.Mutex
	srcs   map[string]*source.Source

	events *amber.EventsService
}

// NewDaemon creates a Daemon
func NewDaemon(store string, pkgfs source.PkgfsDir, events *amber.EventsService) (*Daemon, error) {
	d := &Daemon{
		store:  store,
		pkgfs:  pkgfs,
		srcs:   make(map[string]*source.Source),
		events: events,
	}

	// Ignore if the directory doesn't exist
	srcs, err := loadSourcesFromPath(store)
	if err != nil && !os.IsNotExist(err) {
		return nil, err
	}

	for _, src := range srcs {
		if src.Enabled() {
			if err = d.addSource(src); err != nil {
				return nil, err
			}
		}
	}

	return d, nil
}

// loadSourcesFromPath loads sources from a directory, where each source gets
// it's own directory.  The actual directory structure is source dependent.
func loadSourcesFromPath(dir string) ([]*source.Source, error) {
	files, err := ioutil.ReadDir(dir)
	if err != nil {
		return nil, err
	}

	srcs := make([]*source.Source, 0, len(files))
	for _, f := range files {
		p := filepath.Join(dir, f.Name())
		src, err := source.LoadSourceFromPath(p)
		if err != nil {
			return nil, err
		}
		srcs = append(srcs, src)
	}

	return srcs, nil
}

func (d *Daemon) addToActiveSrcs(s *source.Source) {
	d.muSrcs.Lock()
	defer d.muSrcs.Unlock()

	id := s.GetId()

	if oldSource, ok := d.srcs[id]; ok {
		log.Printf("overwriting active source: %s", id)
		oldSource.Close()
	}

	s.Start()
	d.srcs[id] = s

	log.Printf("added source: %s", id)
}

// AddSource is called to add a Source that can be used to get packages.
func (d *Daemon) AddSource(cfg *amber.SourceConfig) error {
	// Make sure the id is safe to be written to disk.
	store := filepath.Join(d.store, url.PathEscape(cfg.Id))

	// if enabled/disabled is not set, default to disabled
	if cfg.StatusConfig == nil {
		cfg.StatusConfig = &amber.StatusConfig{Enabled: false}
	}
	src, err := source.NewSource(store, cfg)
	if err != nil {
		log.Printf("failed to create TUF source: %v: %s", cfg.Id, err)
		return err
	}

	// Save the config.
	if err := src.Save(); err != nil {
		log.Printf("failed to save TUF config %v: %s", cfg.Id, err)
		return err
	}

	if !src.Enabled() {
		d.muSrcs.Lock()
		if oldSource, ok := d.srcs[cfg.Id]; ok {
			oldSource.Close()
			delete(d.srcs, cfg.Id)
		}
		d.muSrcs.Unlock()
		return nil
	}

	return d.addSource(src)
}

func (d *Daemon) DisableSource(srcID string) error {
	d.muSrcs.Lock()
	defer d.muSrcs.Unlock()

	// Nothing to do if the source is already disabled.
	if _, ok := d.srcs[srcID]; !ok {
		return nil
	}

	// Persist the disabled bit.
	if _, err := d.setSrcEnablementLocked(srcID, false); err != nil {
		return err
	}

	// Remove the source from the running service.
	d.srcs[srcID].Close()
	delete(d.srcs, srcID)

	return nil
}

func (d *Daemon) EnableSource(srcID string) error {
	if err := d.enableSource(srcID); err != nil {
		return err
	}
	metrics.SetTargetChannel(srcID)
	return nil
}
func (d *Daemon) enableSource(srcID string) error {
	d.muSrcs.Lock()
	defer d.muSrcs.Unlock()

	// Nothing to do if the source is already enabled.
	if src, ok := d.srcs[srcID]; ok {
		cfg := src.GetConfig()
		log.Printf("already enabled source: %s %v", srcID, cfg.RepoUrl)
		return nil
	}

	// Persist the enabled bit.
	src, err := d.setSrcEnablementLocked(srcID, true)
	if err != nil {
		// logged in control server / other callers
		return err
	}

	// Add the source to the running service.
	src.Start()
	d.srcs[srcID] = src

	cfg := src.GetConfig()
	log.Printf("enabled source: %s %v", srcID, cfg.RepoUrl)

	return nil
}

func (d *Daemon) setSrcEnablementLocked(srcID string, enabled bool) (*source.Source, error) {
	src, err := d.getSourceLocked(srcID)
	if err != nil {
		return nil, err
	}

	src.SetEnabled(enabled)
	if err := src.Save(); err != nil {
		return nil, err
	}
	return src, nil
}

func (d *Daemon) addSource(src *source.Source) error {
	cfg := src.GetConfig()
	if cfg == nil {
		return fmt.Errorf("source does not have a config")
	}

	// If we made it to this point, we're ready to actually add the source.
	d.addToActiveSrcs(src)

	log.Printf("added TUF source %s %v\n", cfg.Id, cfg.RepoUrl)
	metrics.SetTargetChannel(cfg.Id)

	return nil
}

func (d *Daemon) RemoveSource(id string) (amber.Status, error) {
	// If this method succeeds, the source should be removed from the
	// running service and not be loaded after a service restart. Delete
	// the config file before removing the source from the service to
	// ensure this behavior.
	s, err := d.removeSource(id)
	if err != nil {
		return amber.StatusErr, nil
	} else if s == nil {
		return amber.StatusErrNotFound, nil
	}

	s.Close()

	err = s.Delete()
	if err != nil {
		log.Printf("unable to remove Source from disk: %v\n", err)
	}
	log.Printf("removed source: %s", id)

	return amber.StatusOk, nil
}

func (d *Daemon) removeSource(id string) (*source.Source, error) {
	d.muSrcs.Lock()
	defer d.muSrcs.Unlock()

	s, err := d.getSourceLocked(id)
	if err != nil {
		log.Printf("source %q not found: %s", id, err)
		return nil, nil
	}

	err = s.DeleteConfig()
	if err != nil {
		log.Printf("unable to remove source config from disk: %v", err)
		return nil, err
	}

	delete(d.srcs, id)

	return s, nil
}

// getSourceLocked gets a source, either an enabled or disabled one. It is the
// responsibility of the caller to hold muSrcs when calling.
func (d *Daemon) getSourceLocked(srcID string) (*source.Source, error) {
	src, ok := d.srcs[srcID]
	if ok {
		return src, nil
	}

	allSrcs, err := loadSourcesFromPath(d.store)
	if err != nil {
		return nil, err
	}

	for _, src := range allSrcs {
		if src.GetId() == srcID {
			return src, nil
		}
	}

	return nil, os.ErrNotExist
}

func (d *Daemon) GetActiveSources() map[string]*source.Source {
	d.muSrcs.Lock()
	defer d.muSrcs.Unlock()

	srcs := make(map[string]*source.Source)
	for key, value := range d.srcs {
		srcs[key] = value
	}
	return srcs
}

func (d *Daemon) GetSources() map[string]*source.Source {
	srcs := d.GetActiveSources()
	d.muSrcs.Lock()
	defer d.muSrcs.Unlock()

	// load any sources from disk that we may know about, but are not currently
	// using
	allSrcs, err := loadSourcesFromPath(d.store)
	if err != nil {
		log.Printf("couldn't load sources from disk: %s", err)
	} else {
		// don't override any in-memory entries
		for _, src := range allSrcs {
			if _, ok := srcs[src.GetId()]; !ok {
				srcs[src.GetId()] = src
			}
		}
	}

	return srcs
}

func (d *Daemon) Update() {
	atonce.Do("daemon.Update", "", func() error {
		for id, src := range d.GetActiveSources() {
			if err := src.Update(); err != nil {
				log.Printf("daemon: error updating source %s: %s", id, err)
			}
		}
		return nil
	})
}

func (d *Daemon) UpdateIfStale() {
	atonce.Do("daemon.UpdateIfStale", "", func() error {
		for id, src := range d.GetActiveSources() {
			if err := src.UpdateIfStale(); err != nil {
				log.Printf("daemon: error updating source %s: %s", id, err)
			}
		}
		return nil
	})
}

func (d *Daemon) MerkleFor(name, version, merkle string) (string, int64, error) {
	// Temporary-ish solution to avoid failing/pulling incorrectly updated
	// packages. We need an index into TUF metadata in order to capture appropriate
	// length information.
	if len(merkle) == 64 {
		return merkle, -1, nil
	}

	errs := []error{}

	for _, src := range d.GetActiveSources() {
		m, l, err := src.MerkleFor(name, version)
		if err != nil {
			if err != source.ErrUnknownPkg {
				log.Printf("daemon: error checking source for updates %s", err)
				errs = append(errs, err)
			}

			continue
		}
		return m, l, nil
	}

	if len(errs) == 0 {
		return "", 0, fmt.Errorf("merkle not found for package %s/%s", name, version)
	}

	errStrings := []string{}
	for _, err := range errs {
		errStrings = append(errStrings, err.Error())
	}
	errMsg := strings.Join(errStrings, ", ")

	return "", 0, fmt.Errorf("error finding merkle for package %s/%s: %s", name, version, errMsg)
}

func (d *Daemon) GetPkg(merkle string, length int64) error {
	// TODO(raggi): the fetching of content should preference the source from which
	// the update is sought so as to not unfairly bias fetching from an aribtrarily
	// "first" source.

	err := d.fetchInto(merkle, length, d.pkgfs.PkgInstallDir())
	if err != nil {
		// If the package already existed but was missing the meta FAR (or the
		// meta FAR wasn't indexed), it may now be valid and readable.
		if _, e := os.Stat(filepath.Join(d.pkgfs.VersionsDir(), merkle)); e == nil {
			return nil
		}

		// If the needs dir now exists, ignore a failure to write the meta FAR
		// and move on to processing the package's needs.
		if _, e := os.Stat(filepath.Join(d.pkgfs.PkgNeedsDir(), merkle)); e == nil {
			err = nil
		}
	}
	if err != nil {
		log.Printf("error fetching pkg %q: %s", merkle, err)
		return err
	}

	needsDir, err := os.Open(filepath.Join(d.pkgfs.PkgNeedsDir(), merkle))
	if os.IsNotExist(err) {
		// Package is fully installed already
		return nil
	}
	defer needsDir.Close()

	neededBlobs, err := needsDir.Readdirnames(-1)
	if err != nil {
		return err
	}
	for len(neededBlobs) > 0 {
		for _, blob := range neededBlobs {
			// TODO(raggi): switch to using the needs paths for install
			err := d.fetchInto(blob, -1, d.pkgfs.BlobInstallDir())
			if err != nil {
				return err
			}
		}

		neededBlobs, err = needsDir.Readdirnames(-1)
		if err != nil {
			return err
		}
	}

	// If the package is now readable, we fulfilled all needs, and life is good
	if _, e := os.Stat(filepath.Join(d.pkgfs.VersionsDir(), merkle)); e == nil {
		return nil
	}

	// XXX(raggi): further triage as to the cause of, and recovery from this condition required:
	log.Printf("error fetching pkg %q: %v - package was incomplete after all needs fulfilled", merkle, err)
	if err == nil {
		err = fmt.Errorf("package install incomplete")
	}
	return err
}

func (d *Daemon) fetchInto(merkle string, length int64, outputDir string) error {
	return atonce.Do("fetchInto", merkle, func() error {
		var err error
		for _, source := range d.GetActiveSources() {
			err = source.FetchInto(merkle, length, outputDir)
			if err == nil || os.IsExist(err) {
				return err
			}
			if err, ok := err.(*zx.Error); ok && err.Status == zx.ErrNoSpace {
				for _, key := range d.events.BindingKeys() {
					if p, ok := d.events.EventProxyFor(key); ok {
						log.Printf("daemon: blobfs is out of space")
						if err := p.OnOutOfSpace(); err != nil {
							log.Printf("daemon: OnOutOfSpace failed: %v", err)
						}
					}
				}
				return err
			}
		}
		return fmt.Errorf("not found in %d active sources. last error: %s", len(d.GetActiveSources()), err)
	})
}

func (d *Daemon) GC() error {
	// Garbage collection is done by trying to unlink a particular control
	// file exposed by pkgfs.
	return os.Remove(PackageGarbageDir)
}

func (d *Daemon) OpenRepository(config *pkg.RepositoryConfig) (source.Repository, error) {
	return source.OpenRepository(config, d.pkgfs)
}
