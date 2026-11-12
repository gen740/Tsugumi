# Ruby Cards use ordinary Hashes, Arrays and value objects with #to_h. The
# native adapter validates JSON against each port's FlatBuffers contract.
require 'json'

module Tsugumi
  class Failure < StandardError
    attr_reader :kind
    def initialize(kind, message)
      @kind = kind
      super(message)
    end
  end

  # Binary strings are explicit byte vectors; text never silently changes
  # encoding. FlatBuffers then checks the destination vector's scalar type.
  def self.bytes(value)
    raise ArgumentError, 'bytes expects a String' unless value.is_a?(String)
    value.bytes
  end

  def self.text(value)
    unless value.is_a?(String) && [Encoding::UTF_8, Encoding::US_ASCII].include?(value.encoding) && value.valid_encoding?
      raise ArgumentError, 'text must be valid UTF-8; use Tsugumi.bytes for binary strings'
    end
    value
  end

  def self.json_value(value, depth = 0)
    raise ArgumentError, 'value nesting is too deep' if depth > 100
    case value
    when Hash
      value.each_with_object({}) do |(key, item), out|
        key = key.to_s if key.is_a?(Symbol)
        key = text(key)
        raise ArgumentError, "duplicate field #{key}" if out.key?(key)
        out[key] = json_value(item, depth + 1)
      end
    when Array then value.map { |item| json_value(item, depth + 1) }
    when String then text(value)
    when Integer, TrueClass, FalseClass, NilClass then value
    when Float then value # NaN and the infinities cross as the JSON face spells them
    else
      raise ArgumentError, "unsupported value #{value.class}; use a Hash, Array or #to_h value object" unless value.respond_to?(:to_h)
      object = value.to_h
      raise ArgumentError, '#to_h must return a Hash' unless object.is_a?(Hash)
      json_value(object, depth + 1)
    end
  end

  class Context
    def initialize
      @owner = Thread.current
      @lifetime = { active: true }
      @info = TsugumiHost.info.freeze
      @params = JSON.parse(TsugumiHost.params, allow_nan: true)
      @inputs, @kinds = {}, {}
      TsugumiHost.inputs.each do |name, kind, json|
        @inputs[name] = JSON.parse(json, allow_nan: true)
        @kinds[name] = kind
      end
    end

    def close
      @lifetime[:active] = false
    end

    def check
      raise Failure.new('worker_failed', 'Card context has expired or belongs to another thread') unless @lifetime[:active] && Thread.current == @owner
    end

    %w[run_id card invocation working_directory temporary_directory output_directory].each do |name|
      define_method(name) { check; @info.fetch(name) }
    end

    def params
      check
      @params
    end

    # A missing optional port returns nil. A present message is a Hash; nil
    # fields in that Hash are distinct from a port that was not produced.
    def input(name)
      check
      @inputs[name.to_s]
    end

    def path(name)
      check
      name = name.to_s
      return nil unless @inputs.key?(name)
      raise ArgumentError, "#{name} is not a File or Directory port" unless [1, 2].include?(@kinds[name])
      @inputs.fetch(name).fetch('value')
    end

    def output(name, value)
      check
      value = Tsugumi.json_value(value)
      raise ArgumentError, 'an output message must be a Hash or #to_h value object; omit the call to leave a port unproduced' unless value.is_a?(Hash)
      TsugumiHost.output(name.to_s, JSON.generate(value, allow_nan: true))
    rescue ArgumentError, JSON::GeneratorError => e
      raise Failure.new('output_invalid', e.message)
    end

    def display_text(value)
      check
      TsugumiHost.display_text(Tsugumi.text(value))
    end

    def display_markdown(value)
      check
      TsugumiHost.display_markdown(Tsugumi.text(value))
    end

    def display_scalar(value, label = '')
      check
      raise ArgumentError, 'display scalar must be a finite number' unless value.is_a?(Numeric) && value.finite?
      TsugumiHost.display_scalar(value, Tsugumi.text(label))
    end

    # series: [{ id:, label:, x: [...], y: [...] }, ...]; label defaults to id.
    def display_line_chart(series)
      check
      rows = series.map do |one|
        one = one.to_h.transform_keys(&:to_s)
        id = Tsugumi.text(one.fetch('id').to_s)
        x = one.fetch('x').map { |v| Float(v) }
        y = one.fetch('y').map { |v| Float(v) }
        raise ArgumentError, "series #{id}: x and y differ in length" unless x.length == y.length
        [id, Tsugumi.text((one['label'] || id).to_s), x.pack('d*'), y.pack('d*')]
      end
      TsugumiHost.display_line_chart(rows)
    end

    # A 3D point cloud; colors are 0xRRGGBB integers, one per point, or nil.
    def display_points(x, y, z, colors = nil)
      check
      x, y, z = [x, y, z].map { |axis| axis.map { |v| Float(v) } }
      raise ArgumentError, 'x, y and z differ in length' unless x.length == y.length && y.length == z.length
      unless colors.nil?
        colors = colors.map { |c| Integer(c) }
        raise ArgumentError, 'colors need one 0xRRGGBB value per point' unless colors.length == x.length && colors.all? { |c| c.between?(0, 0xFFFFFF) }
      end
      TsugumiHost.display_points(x.pack('f*'), y.pack('f*'), z.pack('f*'), colors&.pack('L*'))
    end

    def display_table(columns, rows)
      check
      cells = rows.map { |row| row.map { |value| value.is_a?(String) ? Tsugumi.text(value) : JSON.generate(Tsugumi.json_value(value)) } }
      TsugumiHost.display_table(columns.map { |name| Tsugumi.text(name.to_s) }, cells)
    end

    def display_image(bytes, media_type: 'image/png', alt: '')
      check
      raise ArgumentError, 'image data must be a binary String' unless bytes.is_a?(String)
      TsugumiHost.display_image(Tsugumi.text(media_type), bytes, Tsugumi.text(alt))
    end

    def display_module(name, value)
      check
      TsugumiHost.display_module(Tsugumi.text(name), JSON.generate(Tsugumi.json_value(value)))
    end

    def display_plotly(figure)
      display_module('plotly', figure)
    end

    def display_vegalite(spec)
      display_module('vega-lite', spec)
    end

    def log(message)
      check
      TsugumiHost.log(Tsugumi.text(message))
    end

    def cancelled?
      check
      TsugumiHost.cancelled?
    end

    def remaining_ms
      check
      value = TsugumiHost.remaining_ms
      value < 0 ? nil : value
    end

    def fail(kind, message)
      check
      raise Failure.new(kind, message)
    end
  end

  class Card; end

  def self.guard(report: true)
    before = Thread.list
    ctx = report ? Context.new : nil
    begin
      yield ctx
      if (Thread.list - before).any?(&:alive?)
        raise Failure.new('worker_failed', 'join Ruby threads before returning from a Card')
      end
      true
    rescue Exception => error # SystemExit and syntax errors also fail the invocation.
      warn error.full_message
      if report
        kind = error.is_a?(Failure) ? error.kind : 'worker_failed'
        TsugumiHost.fail(kind, "#{error.class}: #{error.message}")
      end
      false
    ensure
      ctx&.close
      (Thread.list - before).each { |thread| thread.kill; thread.join(0.1) }
      $stdout.flush
      $stderr.flush
    end
  end

  def self._load(directory)
    $stdout.sync = true
    $stderr.sync = true
    guard(report: false) do
      file = File.join(directory, 'main.rb')
      Card.class_eval(File.read(file, encoding: Encoding::UTF_8), file, 1)
      @card = Card.new
      raise 'main(ctx) is not defined' unless @card.respond_to?(:main)
    end
  end

  def self._run_start
    return true unless @card.respond_to?(:on_run_start)
    guard { |ctx| @card.on_run_start(ctx) }
  end

  def self._invoke
    guard { |ctx| @card.main(ctx) }
  end
end
