# Evaluated with restricted filesystem access and without IFD.
{
  scope,
  viewsOnly ? false,
  expressionOnly ? false,
}:
let
  argumentsFor =
    entry:
    let
      dir = /. + entry.definition;
      file = dir + "/params.nix";
    in
    {
      params = if builtins.pathExists file then import file else { };
      inputs = builtins.listToAttrs (
        map (id: {
          name = id;
          value = cards.${id}.exports or { };
        }) entry.deps
      );
    };
  cards = builtins.listToAttrs (
    map (entry: {
      name = entry.id;
      value =
        let
          dir = /. + entry.definition;
        in
        if builtins.pathExists (dir + "/default.nix") then import dir (argumentsFor entry) else { };
    }) scope.cards
  );
  arguments = argumentsFor scope.self;
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
  entries =
    if viewsOnly then
      [ ]
    else if executionList then
      run
    else
      [ run ];
  isDerivation = v: builtins.isAttrs v && (v.type or null) == "derivation";
  normalize =
    entry:
    if isDerivation entry then
      {
        label = "";
        run = entry;
      }
    else if
      executionList
      && builtins.isAttrs entry
      &&
        builtins.attrNames entry == [
          "label"
          "run"
        ]
      && builtins.isString entry.label
      && isDerivation entry.run
    then
      entry
    else
      throw "each run implementation must be a derivation; only run list entries can have labels";
  runs = map normalize entries;
  declarations = self.views or [ ];
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
else if !builtins.isList declarations then
  throw "Card views must be a list"
else
  {
    inherit executionList;
    implementationCount = builtins.length entries;
    implementationLabels = builtins.unsafeDiscardStringContext (
      builtins.toJSON (map (r: r.label) runs)
    );
    # The C++ evaluator transfers this string and its derivation context to
    # the trusted plan builder. It never passes a Card value through as code.
    plan = builtins.toJSON {
      runs = map (r: "${r.run}") runs;
      views = map viewOf declarations;
    };
  }
