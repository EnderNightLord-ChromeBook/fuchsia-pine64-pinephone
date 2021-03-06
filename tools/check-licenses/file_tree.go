// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package checklicenses

import (
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"sync"

	"golang.org/x/sync/errgroup"
)

// NewFileTree returns an instance of FileTree, given the input configuration file.
func NewFileTree(config *Config, metrics *Metrics) *FileTree {
	var eg errgroup.Group
	var recursiveHelper func(string) error
	var file_tree FileTree
	file_tree.Init()
	root := config.BaseDir

	recursiveHelper = func(root string) error {
		return filepath.Walk(root, func(path string, info os.FileInfo, err error) error {
			if err != nil {
				fmt.Printf("error walking the path %q: %v\n", root, err)
				return err
			}
			if info.IsDir() {
				for _, skipDir := range config.SkipDirs {
					if info.Name() == skipDir || path == skipDir {
						log.Printf("skipping a dir without errors: %s", info.Name())
						return filepath.SkipDir
					}
				}

				for _, customProjectLicense := range config.CustomProjectLicenses {
					if path == customProjectLicense.ProjectRoot {
						metrics.increment("num_single_license_files")
						// TODO(omerlevran): Fix the directory and file_root having to repeat a
						// directory.
						file_tree.addSingleLicenseFile(path, customProjectLicense.LicenseLocation)
						break
					}
				}

				// Instead of using filepath.Walk to traverse the directory tree,
				// we will instead call this same function recursively on each
				// subtree, and return "filepath.SkipDir" to prevent filepath.Walk
				// from entering the child directories. This allows us to parallelize
				// the walk procedure.
				//
				// Special case: In the first loop, root == path.
				// Returning filepath.SkipDir on that loop would cancel the entire
				// walk procedure, and no files would be processed.
				if root != path {
					path := path
					eg.Go(func() error {
						return recursiveHelper(path)
					})
					return filepath.SkipDir
				}
				return nil
			} else {
				for _, skipFile := range config.SkipFiles {
					if strings.ToLower(info.Name()) == strings.ToLower(skipFile) {
						log.Printf("skipping a file without errors: %s", info.Name())
						return nil
					}
				}
			}
			if isSingleLicenseFile(info.Name(), config.SingleLicenseFiles) {
				metrics.increment("num_single_license_files")
				file_tree.addSingleLicenseFile(path, filepath.Base(path))
			} else {
				if isValidExtension(path, config) {
					metrics.increment("num_non_single_license_files")
					file_tree.addFile(path)
				} else {
					metrics.increment("num_extensions_excluded")
				}
			}
			return nil
		})
	}

	eg.Go(func() error {
		return recursiveHelper(root)
	})

	if err := eg.Wait(); err != nil {
		// TODO(jcecil): This must be an error.
		fmt.Printf("error while traversing directory '%v", err)
		return nil
	}

	return &file_tree
}

// FileTree is an in memory representation of the state of the repository.
type FileTree struct {
	name               string
	children           map[string]*FileTree
	files              []string
	singleLicenseFiles map[string][]*License
	parent             *FileTree

	sync.RWMutex
}

func (license_file_tree *FileTree) Init() {
	license_file_tree.children = make(map[string]*FileTree)
	license_file_tree.singleLicenseFiles = make(map[string][]*License)
}

func (file_tree *FileTree) getSetCurr(path string) *FileTree {
	children := strings.Split(filepath.Dir(path), "/")
	curr := file_tree
	currBkp := curr
	curr.Lock()
	for _, child := range children {
		if _, found := curr.children[child]; !found {
			curr.children[child] = &FileTree{name: child, parent: curr}
			curr.children[child].Init()
		}
		curr = curr.children[child]
	}
	currBkp.Unlock()
	return curr
}

func (file_tree *FileTree) addFile(path string) {
	curr := file_tree.getSetCurr(path)
	curr.files = append(curr.files, filepath.Base(path))
}

func (file_tree *FileTree) addSingleLicenseFile(path string, base string) {
	curr := file_tree.getSetCurr(path)
	curr.singleLicenseFiles[base] = []*License{}
}

func (file_tree *FileTree) getProjectLicense(path string) *FileTree {
	curr := file_tree
	var gold *FileTree
	pieces := strings.Split(filepath.Dir(path), "/")
	for _, piece := range pieces {
		if len(curr.singleLicenseFiles) > 0 {
			gold = curr
		}
		curr.RLock()
		if _, found := curr.children[piece]; !found {
			curr.RUnlock()
			break
		}
		currNext := curr.children[piece]
		curr.RUnlock()
		curr = currNext
	}
	if len(pieces) > 1 && len(curr.singleLicenseFiles) > 0 {
		gold = curr
	}
	return gold
}

func (file_tree *FileTree) getPath() string {
	var arr []string
	curr := file_tree
	for {
		if curr == nil {
			break
		}
		arr = append(arr, curr.name)
		curr = curr.parent
	}
	var sb strings.Builder
	for i := len(arr) - 1; i >= 0; i-- {
		if len(arr[i]) == 0 {
			continue
		}
		fmt.Fprintf(&sb, "%s/", arr[i])
	}
	return sb.String()
}

func (file_tree *FileTree) getSingleLicenseFileIterator() <-chan *FileTree {
	ch := make(chan *FileTree, 1)
	go func() {
		var curr *FileTree
		var q []*FileTree
		q = append(q, file_tree)
		var pos int
		for len(q) > 0 {
			pos = len(q) - 1
			curr = q[pos]
			q = q[:pos]
			if len(curr.singleLicenseFiles) > 0 {
				ch <- curr
			}
			curr.RLock()
			for _, child := range curr.children {
				q = append(q, child)
			}
			curr.RUnlock()
		}
		close(ch)
	}()
	return ch
}

func (file_tree *FileTree) getFileIterator() <-chan string {
	ch := make(chan string, 1)
	go func() {
		var curr *FileTree
		var q []*FileTree
		q = append(q, file_tree)
		var pos int
		for len(q) > 0 {
			pos = len(q) - 1
			curr = q[pos]
			q = q[:pos]
			base := curr.getPath()
			for _, file := range curr.files {
				ch <- base + file
			}
			curr.RLock()
			for _, child := range curr.children {
				q = append(q, child)
			}
			curr.RUnlock()
		}
		close(ch)
	}()
	return ch
}

func isValidExtension(path string, config *Config) bool {
	extension := filepath.Ext(path)
	if len(extension) == 0 {
		return false
	}
	_, found := config.TextExtensions[extension[1:]]
	return found
}

func readFromFile(path string, max_read_size int64) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	data := make([]byte, max_read_size)
	count, err := file.Read(data)
	if err != nil {
		// TODO(solomonkinard) symlinks not found e.g. integration/fuchsia/infra/test_durations/README.md
		return nil, err
	}
	data = data[:count]
	return data, nil
}

func isSingleLicenseFile(name string, files []string) bool {
	name = strings.ToLower(name)
	for _, f := range files {
		// example of file: LICENSE, LICENSE-THIRD-PARTY, ...
		if strings.HasPrefix(name, f) {
			return true
		}
	}
	return false
}
