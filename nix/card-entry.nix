# Evaluated with restricted filesystem access and without IFD.
recordExport:
{
  scope,
  viewsOnly ? false,
  expressionOnly ? false,
}:
let
  argumentsFor =
    track: entry:
    let
      dir = /. + entry.definition;
      file = dir + "/params.nix";
    in
    {
      params = if builtins.pathExists file then import file else { };
      inputs = builtins.listToAttrs (
        map (dep: {
          inherit (dep) name;
          value = mergeExports track dep;
        }) entry.deps
      );
    };
  # inputs.<name> of a subgraph Card holds the exports of every inner Card
  # wired to its boundary outputs, merged; one attribute from two of them is
  # an error.
  mergeExports =
    track: dep:
    let
      exports = map (
        key:
        let
          values = cards.${key}.exports or { };
        in
        if track && builtins.isAttrs values then builtins.mapAttrs (recordExport key) values else values
      ) dep.keys;
    in
    if builtins.length exports == 1 then
      builtins.head exports
    else
      builtins.foldl' (
        merged: next:
        if !(builtins.isAttrs next) then
          throw "inputs.${dep.name}: the exports of a subgraph's Cards must be attribute sets"
        else
          let
            duplicates = builtins.filter (name: merged ? ${name}) (builtins.attrNames next);
          in
          if duplicates != [ ] then
            throw "inputs.${dep.name}: ${builtins.head duplicates} is exported by more than one Card"
          else
            merged // next
      ) { } exports;
  cards = builtins.listToAttrs (
    map (entry: {
      name = entry.key;
      value =
        let
          dir = /. + entry.definition;
        in
        if builtins.pathExists (dir + "/default.nix") then import dir (argumentsFor false entry) else { };
    }) scope.cards
  );
  arguments = argumentsFor (!viewsOnly && !expressionOnly) scope.self;
  self = import (/. + scope.self.definition) arguments;
  unknown = builtins.filter (
    key:
    !(builtins.elem key [
      "run"
      "views"
      "exports"
      "api"
    ])
  ) (builtins.attrNames self);
  run = self.run or (throw "a Card needs run");
  executionList = viewsOnly || builtins.isList run;
  isDerivation = v: builtins.isAttrs v && (v.type or null) == "derivation";
  checkEntry =
    entry:
    if
      builtins.isAttrs entry
      &&
        builtins.attrNames entry == [
          "label"
          "run"
        ]
      && isDerivation entry.run
    then
      entry
    else
      throw runEntryError;
  runs =
    if viewsOnly then
      [ ]
    else if executionList then
      map checkEntry run
    else if isDerivation run then
      [
        {
          label = "";
          inherit run;
        }
      ]
    else
      throw "run must be a derivation or a list of labelled derivations";
  declarations = self.views or [ ];
  runPaths = map (r: "${r.run}") runs;
  labelOf = r: if builtins.isString r.label then r.label else throw runEntryError;
  runEntryError = "each run list entry must have a string label and a derivation";
  viewOf =
    view:
    if
      builtins.isAttrs view
      &&
        builtins.attrNames view == [
          "blob"
          "name"
          "type"
        ]
      && builtins.isString view.name
      && builtins.elem view.type [
        "dawn"
        "html"
      ]
      && (
        builtins.isPath view.blob
        || isDerivation view.blob
        || (builtins.isString view.blob && builtins.hasContext view.blob)
      )
    then
      let
        blob = "${view.blob}";
        store = "${builtins.storeDir}/";
      in
      if
        builtins.substring 0 (builtins.stringLength store) blob == store
        && !(builtins.elem ".." (builtins.split "/" blob))
      then
        {
          inherit (view) name type;
          inherit blob;
        }
      else
        throw "a Card view blob must refer to a file in the Nix store"
    else
      throw "a Card view must declare { name; type = dawn or html; blob = a file path; }";
in
if expressionOnly then
  arguments
else if unknown != [ ] then
  throw "unknown Card attributes: ${toString unknown} (known: run views exports api)"
else
  {
    inherit executionList runPaths;
    implementationCount = builtins.length runs;
    implementationLabels = builtins.unsafeDiscardStringContext (builtins.toJSON (map labelOf runs));
    # The C++ evaluator transfers this string and its derivation context to
    # the trusted plan builder. It never passes a Card value through as code.
    plan = builtins.toJSON {
      runs = runPaths;
      views =
        if builtins.isList declarations then map viewOf declarations else throw "Card views must be a list";
    };
  }
