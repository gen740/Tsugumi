# Ruby Cards use ordinary Hashes, Arrays and value objects with #to_h. The
# native adapter validates JSON against each port's FlatBuffers contract.

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

  class CallScope
    def initialize
      @owner = Thread.current
      @lifetime = { active: true }
      @info = TsugumiHost.info
      @params = JSON.parse(TsugumiHost.params, allow_nan: true)
    end

    def info
      check
      @info
    end

    def close
      @lifetime[:active] = false
    end

    def check
      raise Failure.new('worker_failed', 'Card call has expired or belongs to another thread') unless @lifetime[:active] && Thread.current == @owner
    end

    def params
      check
      @params
    end

    def output(name, value)
      check
      value = Tsugumi.json_value(value)
      raise ArgumentError, 'an output message must be a Hash or #to_h value object; omit the port to leave it unproduced' unless value.is_a?(Hash)
      TsugumiHost.output(name.to_s, JSON.generate(value, allow_nan: true))
    rescue ArgumentError, JSON::GeneratorError => e
      raise Failure.new('output_invalid', e.message)
    end

    def text(value)
      check
      TsugumiHost.display_text(Tsugumi.text(value))
    end

    def markdown(value)
      check
      TsugumiHost.display_markdown(Tsugumi.text(value))
    end

    def scalar(value, label = '')
      check
      raise ArgumentError, 'display scalar must be a finite number' unless value.is_a?(Numeric) && value.finite?
      TsugumiHost.display_scalar(value, Tsugumi.text(label))
    end

    # series: [{ id:, label:, x: [...], y: [...] }, ...]; label defaults to id.
    def line_chart(series)
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
    def points(x, y, z, colors = nil)
      check
      x, y, z = [x, y, z].map { |axis| axis.map { |v| Float(v) } }
      raise ArgumentError, 'x, y and z differ in length' unless x.length == y.length && y.length == z.length
      unless colors.nil?
        colors = colors.map { |c| Integer(c) }
        raise ArgumentError, 'colors need one 0xRRGGBB value per point' unless colors.length == x.length && colors.all? { |c| c.between?(0, 0xFFFFFF) }
      end
      TsugumiHost.display_points(x.pack('f*'), y.pack('f*'), z.pack('f*'), colors&.pack('L*'))
    end

    def table(columns, rows)
      check
      cells = rows.map { |row| row.map { |value| value.is_a?(String) ? Tsugumi.text(value) : JSON.generate(Tsugumi.json_value(value)) } }
      TsugumiHost.display_table(columns.map { |name| Tsugumi.text(name.to_s) }, cells)
    end

    def image(bytes, media_type: 'image/png', alt: '')
      check
      raise ArgumentError, 'image data must be a binary String' unless bytes.is_a?(String)
      TsugumiHost.display_image(Tsugumi.text(media_type), bytes, Tsugumi.text(alt))
    end

    def module(name, value)
      check
      TsugumiHost.display_module(Tsugumi.text(name), JSON.generate(Tsugumi.json_value(value)))
    end

    def plotly(figure)
      self.module('plotly', figure)
    end

    def vegalite(spec)
      self.module('vega-lite', spec)
    end

    def log(message)
      check
      TsugumiHost.log(Tsugumi.text(message))
    end
  end

  def self.freeze_input(value)
    case value
    when Hash then value.each { |key, item| key.freeze; freeze_input(item) }
    when Array then value.each { |item| freeze_input(item) }
    end
    value.freeze
  end

  class Inputs
    def initialize(scope)
      @params = Tsugumi.freeze_input(scope.params)
      @kinds = TsugumiHost.inputs.to_h { |name, kind, _| [name, kind] }.freeze
      @values = TsugumiHost.inputs.to_h { |name, _, json| [name, Tsugumi.freeze_input(JSON.parse(json, allow_nan: true))] }.freeze
      freeze
    end
    attr_reader :params
    def [](name) = @values[name.to_s]
    def input(name) = self[name]
    def path(name)
      return nil unless @values.key?(name.to_s)
      raise ArgumentError, "#{name} is not a File or Directory port" unless [1, 2].include?(@kinds[name.to_s])
      self[name].fetch('value')
    end
    def method_missing(name, *args)
      return self[name] if args.empty? && !name.to_s.end_with?('=')
      super
    end
    def respond_to_missing?(name, private_ = false) = @values.key?(name.to_s) || super
  end

  class Report
    def initialize(scope) = @scope = scope
    %i[text markdown scalar line_chart table image points module plotly vegalite log].each do |name|
      define_method(name) { |*args, **options| @scope.public_send(name, *args, **options) }
    end
  end

  class Run
    def initialize(scope) = @scope = scope
    %w[run_id card invocation working_directory temporary_directory output_directory].each do |name|
      define_method(name) { @scope.info.fetch(name) }
    end
    def cancelled? = (@scope.check; TsugumiHost.cancelled?)
    def remaining_ms = (@scope.check; TsugumiHost.remaining_ms)
  end

  def self.call(function, scope)
    args = [Inputs.new(scope), Report.new(scope), Run.new(scope)]
    raise ArgumentError, 'Card functions take inputs, optionally report and run' unless (1..3).cover?(function.arity)
    result = function.call(*args.take(function.arity))
    send_outputs(scope, result)
  end

  def self.send_outputs(scope, result)
    return if result.nil?
    if result.is_a?(Enumerator)
      result.each { |batch| send_outputs(scope, batch) }
      return
    end
    raise ArgumentError, 'return a Hash by output port' unless result.is_a?(Hash)
    result.each { |port, value| scope.output(port, value) }
  end

  private_constant :CallScope


  class Card; end

  def self.guard
    require 'json'
    before = Thread.list
    scope = CallScope.new
    begin
      yield scope
      if (Thread.list - before).any?(&:alive?)
        raise Failure.new('worker_failed', 'join Ruby threads before returning from a Card')
      end
      true
    rescue Exception => error # SystemExit and syntax errors also fail the invocation.
      warn error.full_message
      kind = error.is_a?(Failure) ? error.kind : 'worker_failed'
      TsugumiHost.fail(kind, "#{error.class}: #{error.message}")
      false
    ensure
      scope&.close
      (Thread.list - before).each { |thread| thread.kill; thread.join(0.1) }
      $stdout.flush
      $stderr.flush
    end
  end

  def self._load(directory)
    $stdout.sync = true
    $stderr.sync = true
    guard do
      file = File.join(directory, 'main.rb')
      Card.class_eval(File.read(file, encoding: Encoding::UTF_8), file, 1)
      @card = Card.new
      raise 'main(inputs[, report[, run]]) is not defined' unless @card.respond_to?(:main)
    end
  end

  def self._run_start
    return true unless @card.respond_to?(:on_run_start)
    guard { |scope| call(@card.method(:on_run_start), scope) }
  end

  def self._invoke
    guard do |scope|
      call(@card.method(:main), scope)
      TsugumiHost.finish_outputs
    end
  end
end
