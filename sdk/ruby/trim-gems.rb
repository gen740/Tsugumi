# Keep installed specifications for Bundler and RubyGems, without publishing fields.
require 'rubygems'

root = ARGV.fetch(0)
fields = %w[authors email homepage licenses description summary metadata date files
            test_files extra_rdoc_files rdoc_options cert_chain post_install_message
            signing_key installed_by_version]
pattern = /^  s\.(#{fields.join('|')}) =/
Dir.glob("#{root}/ruby/*/specifications/*.gemspec").each do |path|
  specification = Gem::Specification.load(path)
  File.write(path, specification.to_ruby.lines.reject { |line| pattern.match?(line) }.join)
end
Dir.glob("#{root}/ruby/*/gems/*").each do |gem|
  Dir.glob("#{gem}/*.gemspec").each { |path| File.delete(path) }
  %w[Rakefile Gemfile Gemfile.lock].each do |name|
    path = File.join(gem, name)
    File.delete(path) if File.file?(path)
  end
end
