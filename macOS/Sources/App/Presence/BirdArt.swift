// The tsugumi of the app icon (docs/assets/tsugumi-icon.svg), polygon by
// polygon in icon units, part by part so its wings, head and tail can move.
import CoreGraphics

struct BirdPolygon { let points: [CGPoint]; let color: (Double, Double, Double); let opacity: Double }

enum BirdArt {
  static let legs: [BirdPolygon] = [
    BirdPolygon(points: [CGPoint(x: 468, y: 600), CGPoint(x: 494, y: 604), CGPoint(x: 486, y: 640), CGPoint(x: 470, y: 640)], color: (0.851, 0.761, 0.620), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 474, y: 640), CGPoint(x: 484, y: 640), CGPoint(x: 478, y: 702), CGPoint(x: 472, y: 702)], color: (0.753, 0.553, 0.408), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 472, y: 702), CGPoint(x: 478, y: 694), CGPoint(x: 512, y: 704), CGPoint(x: 476, y: 708)], color: (0.659, 0.455, 0.310), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 472, y: 700), CGPoint(x: 476, y: 708), CGPoint(x: 444, y: 708)], color: (0.557, 0.373, 0.243), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 528, y: 592), CGPoint(x: 554, y: 596), CGPoint(x: 546, y: 636), CGPoint(x: 530, y: 636)], color: (0.804, 0.706, 0.549), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 534, y: 636), CGPoint(x: 544, y: 636), CGPoint(x: 540, y: 700), CGPoint(x: 534, y: 700)], color: (0.690, 0.486, 0.345), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 534, y: 700), CGPoint(x: 540, y: 692), CGPoint(x: 572, y: 700), CGPoint(x: 538, y: 706)], color: (0.659, 0.455, 0.310), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 534, y: 698), CGPoint(x: 538, y: 706), CGPoint(x: 508, y: 706)], color: (0.557, 0.373, 0.243), opacity: 1),
  ]
  static let tail: [BirdPolygon] = [
    BirdPolygon(points: [CGPoint(x: 330, y: 480), CGPoint(x: 340, y: 585), CGPoint(x: 175, y: 600)], color: (0.306, 0.200, 0.125), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 175, y: 600), CGPoint(x: 340, y: 585), CGPoint(x: 215, y: 655)], color: (0.227, 0.149, 0.090), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 175, y: 600), CGPoint(x: 215, y: 655), CGPoint(x: 160, y: 640)], color: (0.184, 0.122, 0.075), opacity: 1),
  ]
  static let body: [BirdPolygon] = [
    BirdPolygon(points: [CGPoint(x: 520, y: 330), CGPoint(x: 655, y: 452), CGPoint(x: 420, y: 400)], color: (0.435, 0.290, 0.173), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 655, y: 452), CGPoint(x: 605, y: 545), CGPoint(x: 420, y: 400)], color: (0.937, 0.878, 0.765), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 420, y: 400), CGPoint(x: 605, y: 545), CGPoint(x: 330, y: 480)], color: (0.886, 0.800, 0.643), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 330, y: 480), CGPoint(x: 605, y: 545), CGPoint(x: 560, y: 610)], color: (0.957, 0.914, 0.831), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 330, y: 480), CGPoint(x: 560, y: 610), CGPoint(x: 470, y: 628)], color: (0.914, 0.847, 0.722), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 330, y: 480), CGPoint(x: 470, y: 628), CGPoint(x: 340, y: 585)], color: (0.831, 0.725, 0.553), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 655, y: 452), CGPoint(x: 690, y: 500), CGPoint(x: 605, y: 545)], color: (0.902, 0.827, 0.690), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 605, y: 545), CGPoint(x: 690, y: 500), CGPoint(x: 560, y: 610)], color: (0.973, 0.937, 0.867), opacity: 1),
  ]
  static let spots: [BirdPolygon] = [
    BirdPolygon(points: [CGPoint(x: 610, y: 470), CGPoint(x: 628, y: 478), CGPoint(x: 612, y: 490)], color: (0.231, 0.153, 0.094), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 575, y: 500), CGPoint(x: 594, y: 505), CGPoint(x: 580, y: 520)], color: (0.231, 0.153, 0.094), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 630, y: 512), CGPoint(x: 648, y: 516), CGPoint(x: 636, y: 532)], color: (0.231, 0.153, 0.094), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 548, y: 540), CGPoint(x: 566, y: 546), CGPoint(x: 552, y: 560)], color: (0.231, 0.153, 0.094), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 596, y: 552), CGPoint(x: 614, y: 556), CGPoint(x: 602, y: 572)], color: (0.231, 0.153, 0.094), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 520, y: 575), CGPoint(x: 536, y: 580), CGPoint(x: 524, y: 592)], color: (0.231, 0.153, 0.094), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 570, y: 590), CGPoint(x: 586, y: 593), CGPoint(x: 576, y: 606)], color: (0.231, 0.153, 0.094), opacity: 1),
  ]
  static let wing: [BirdPolygon] = [
    BirdPolygon(points: [CGPoint(x: 545, y: 402), CGPoint(x: 440, y: 418), CGPoint(x: 455, y: 500)], color: (0.690, 0.404, 0.184), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 545, y: 402), CGPoint(x: 455, y: 500), CGPoint(x: 525, y: 522)], color: (0.788, 0.490, 0.239), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 440, y: 418), CGPoint(x: 298, y: 560), CGPoint(x: 455, y: 500)], color: (0.561, 0.310, 0.133), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 455, y: 500), CGPoint(x: 298, y: 560), CGPoint(x: 525, y: 522)], color: (0.639, 0.361, 0.161), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 298, y: 560), CGPoint(x: 525, y: 522), CGPoint(x: 400, y: 560)], color: (0.435, 0.235, 0.102), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 545, y: 402), CGPoint(x: 525, y: 522), CGPoint(x: 565, y: 470)], color: (0.835, 0.545, 0.286), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 380, y: 470), CGPoint(x: 300, y: 556), CGPoint(x: 410, y: 505)], color: (0.353, 0.188, 0.082), opacity: 0.6),
  ]
  static let head: [BirdPolygon] = [
    BirdPolygon(points: [CGPoint(x: 590, y: 298), CGPoint(x: 665, y: 320), CGPoint(x: 628, y: 366)], color: (0.541, 0.365, 0.212), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 520, y: 330), CGPoint(x: 590, y: 298), CGPoint(x: 628, y: 366)], color: (0.478, 0.322, 0.196), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 665, y: 320), CGPoint(x: 694, y: 356), CGPoint(x: 628, y: 366)], color: (0.604, 0.416, 0.247), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 520, y: 330), CGPoint(x: 628, y: 366), CGPoint(x: 655, y: 452)], color: (0.788, 0.655, 0.478), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 628, y: 366), CGPoint(x: 694, y: 356), CGPoint(x: 688, y: 402)], color: (0.231, 0.153, 0.094), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 628, y: 366), CGPoint(x: 688, y: 402), CGPoint(x: 655, y: 452)], color: (0.953, 0.902, 0.800), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 655, y: 452), CGPoint(x: 688, y: 402), CGPoint(x: 690, y: 500)], color: (0.918, 0.859, 0.741), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 575, y: 338), CGPoint(x: 662, y: 334), CGPoint(x: 668, y: 346), CGPoint(x: 592, y: 352)], color: (0.945, 0.886, 0.769), opacity: 1),
  ]
  static let eye: [BirdPolygon] = [
    BirdPolygon(points: [CGPoint(x: 628, y: 354), CGPoint(x: 641, y: 361), CGPoint(x: 641, y: 376), CGPoint(x: 628, y: 383), CGPoint(x: 615, y: 376), CGPoint(x: 615, y: 361)], color: (0.086, 0.063, 0.043), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 623, y: 360), CGPoint(x: 631, y: 358), CGPoint(x: 628, y: 366)], color: (1.000, 1.000, 1.000), opacity: 0.9),
  ]
  static let beak: [BirdPolygon] = [
    BirdPolygon(points: [CGPoint(x: 694, y: 356), CGPoint(x: 776, y: 394), CGPoint(x: 728, y: 384)], color: (0.180, 0.129, 0.090), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 688, y: 402), CGPoint(x: 694, y: 356), CGPoint(x: 728, y: 384)], color: (0.239, 0.173, 0.122), opacity: 1),
    BirdPolygon(points: [CGPoint(x: 688, y: 402), CGPoint(x: 728, y: 384), CGPoint(x: 776, y: 394)], color: (0.851, 0.643, 0.255), opacity: 1),
  ]
}
