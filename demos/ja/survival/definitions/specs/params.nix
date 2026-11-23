{
  adjustments = [ "" "age" "age + ph.ecog" "age + ph.ecog + ph.karno + wt.loss" ];
  exposure = "female";
  subsets = [ "TRUE" "age < 65" "age >= 65" "ph.ecog == 0" "ph.ecog == 1" "ph.ecog >= 2" ];
}
