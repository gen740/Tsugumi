func nextInputName(_ names: some Sequence<String>) -> String {
  let taken = Set(names)
  var n = 1
  while taken.contains("in\(n)") { n += 1 }
  return "in\(n)"
}
