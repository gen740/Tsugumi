"""Pruned Card dependencies still import and expose their runtime package identity."""
import csv
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]


def trim(root):
    subprocess.run([sys.executable, str(ROOT / 'sdk/codegen/trim_packages.py'), str(root)], check=True)


def test_wheel_import_discovery_and_resources_survive_pruning(tmp_path):
    package = tmp_path / 'sample'
    package.mkdir()
    (package / '__init__.py').write_text('VALUE = 42\n')
    (package / 'data.json').write_text('{"value": 42}')
    for directory in ('tests', 'docs', 'examples'):
        (package / directory).mkdir()
        (package / directory / 'unused.py').write_text('raise RuntimeError("extra")')
    info = tmp_path / 'sample-1.0.dist-info'
    info.mkdir()
    (info / 'licenses').mkdir()
    (info / 'licenses/LICENSE').write_text('unused')
    (info / 'METADATA').write_text('Metadata-Version: 2.4\nName: sample\nVersion: 1.0\n'
        'Requires-Dist: dependency>=1\nProvides-Extra: optional\nAuthor: Author\nLicense: License\n'
        'License-File: licenses/LICENSE\nClassifier: Example\n\nREADME contents\n')
    (info / 'entry_points.txt').write_text('[sample.plugins]\nplugin = sample:VALUE\n')
    with (info / 'RECORD').open('w', newline='') as stream:
        csv.writer(stream).writerows([str(p.relative_to(tmp_path)), '', ''] for p in tmp_path.rglob('*') if p.is_file())
    trim(tmp_path)
    subprocess.run([sys.executable, '-c', '''
import importlib.metadata as m
import importlib.resources as r
import sample
assert sample.VALUE == 42
distribution = m.distribution('sample')
assert distribution.version == '1.0'
assert distribution.requires == ['dependency>=1']
assert next(iter(distribution.entry_points)).load() == 42
assert r.files(sample).joinpath('data.json').read_text() == '{"value": 42}'
assert all(distribution.locate_file(f).is_file() for f in distribution.files)
assert 'Author' not in distribution.metadata and 'License-File' not in distribution.metadata
assert not distribution.metadata.get_payload()
'''], env={**os.environ, 'PYTHONPATH': str(tmp_path)}, check=True)
    assert set(p.name for p in package.iterdir()) <= {'__init__.py', 'data.json', '__pycache__'}
    assert not (info / 'licenses').exists()


def test_gem_import_and_bundler_specification_survive_pruning(tmp_path):
    ruby = shutil.which('ruby')
    if not ruby:
        pytest.skip('Ruby is required')
    api = subprocess.check_output([ruby, '-e', 'print Gem.ruby_api_version'], text=True)
    base = tmp_path / 'ruby' / api
    gem = base / 'gems/sample-1.0'
    (gem / 'lib').mkdir(parents=True)
    (gem / 'lib/sample.rb').write_text('SAMPLE_VALUE = 42\n')
    (gem / 'README.md').write_text('unused')
    (gem / 'LICENSE').write_text('unused')
    (gem / 'test').mkdir()
    (gem / 'test/test_sample.rb').write_text('raise "extra"')
    specs = base / 'specifications'
    specs.mkdir()
    spec = specs / 'sample-1.0.gemspec'
    spec.write_text('''Gem::Specification.new do |s|
  s.name = 'sample'
  s.version = '1.0'
  s.authors = ['Author']
  s.summary = 'Description'
  s.licenses = ['License']
  s.require_paths = ['lib']
  s.required_ruby_version = '>= 2.6'
end
''')
    trim(tmp_path)
    subprocess.run([ruby, str(ROOT / 'sdk/ruby/trim-gems.rb'), str(tmp_path)], check=True)
    (tmp_path / 'Gemfile').write_text('source "https://rubygems.org"\ngem "sample", "1.0"\n')
    (tmp_path / 'Gemfile.lock').write_text('''GEM
  remote: https://rubygems.org/
  specs:
    sample (1.0)

PLATFORMS
  ruby

DEPENDENCIES
  sample (= 1.0)
''')
    subprocess.run([ruby, '-e', '''
require 'rubygems'
require 'bundler/setup'
require 'sample'
raise unless SAMPLE_VALUE == 42
spec = Gem::Specification.find_by_name('sample')
raise unless spec.version.to_s == '1.0'
raise unless spec.required_ruby_version.satisfied_by?(Gem.ruby_version)
raise unless spec.authors.empty? && spec.licenses.empty?
'''], env={**os.environ, 'BUNDLE_GEMFILE': str(tmp_path / 'Gemfile'),
          'BUNDLE_PATH': str(tmp_path), 'BUNDLE_FROZEN': 'true'}, check=True)
    assert not (gem / 'test').exists()
    assert not (gem / 'README.md').exists()
    assert not (gem / 'LICENSE').exists()
