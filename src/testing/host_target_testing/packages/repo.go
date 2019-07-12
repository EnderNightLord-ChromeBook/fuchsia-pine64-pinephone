// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package packages

import (
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path/filepath"

	"fuchsia.googlesource.com/host_target_testing/util"
	"fuchsia.googlesource.com/pm/repo"
)

type Repository struct {
	Dir     string
	targets targets
}

type signed struct {
	Signed targets `json:"signed"`
}

type targets struct {
	Targets map[string]targetFile `json:"targets"`
}

type targetFile struct {
	Custom custom `json:"custom"`
}

type custom struct {
	Merkle string `json:"merkle"`
}

// NewRepository parses the repository from the specified directory. It returns
// an error if the repository does not exist, or it contains malformed metadata.
func NewRepository(dir string) (*Repository, error) {
	log.Printf("creating a repository for %q", dir)

	// The repository may have out of date metadata. This updates the repository to
	// the latest version so TUF won't complain about the data being old.
	repo, err := repo.New(dir)
	if err != nil {
		return nil, err
	}
	if err := repo.CommitUpdates(true); err != nil {
		return nil, err
	}

	repoDir := filepath.Join(dir, "repository")

	// Parse the targets file so we can access packages locally.
	f, err := os.Open(filepath.Join(repoDir, "targets.json"))
	if err != nil {
		return nil, err
	}
	defer f.Close()

	var s signed
	if err = json.NewDecoder(f).Decode(&s); err != nil {
		return nil, err
	}

	return &Repository{
		Dir:     repoDir,
		targets: s.Signed,
	}, nil
}

// NewRepositoryFromTar extracts a repository from a tar.gz, and returns a
// Repository parsed from it. It returns an error if the repository does not
// exist, or contains malformed metadata.
func NewRepositoryFromTar(dst string, src string) (*Repository, error) {
	if err := util.Untar(dst, src); err != nil {
		return nil, fmt.Errorf("failed to extract packages: %s", err)
	}

	return NewRepository(filepath.Join(dst, "amber-files"))
}

// Open a package from the p
func (r *Repository) OpenPackage(path string) (Package, error) {
	if target, ok := r.targets.Targets[path]; ok {
		return newPackage(r, target.Custom.Merkle)
	}
	return Package{}, fmt.Errorf("could not find package: %q", path)

}

func (r *Repository) OpenBlob(merkle string) (*os.File, error) {
	return os.Open(filepath.Join(r.Dir, "blobs", merkle))
}

func (r *Repository) Serve(localHostname string) (*Server, error) {
	return newServer(r.Dir, localHostname)
}
