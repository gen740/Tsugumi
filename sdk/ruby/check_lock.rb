# bundlerEnv uses hashed gemset.nix sources; require every locked dependency
# to be present at exactly that version in the resulting offline environment.
ENV['BUNDLE_FROZEN'] = 'true'
ENV['BUNDLE_IGNORE_CONFIG'] = 'true'
require 'bundler'
Bundler.setup
lock = Bundler::LockfileParser.new(File.read(ARGV.fetch(0)))
lock.specs.each do |spec|
  matches = Gem::Specification.find_all_by_name(spec.name, spec.version.to_s)
  abort "Gemfile.lock dependency is missing from gemset.nix: #{spec.name} #{spec.version}" if matches.empty?
end
