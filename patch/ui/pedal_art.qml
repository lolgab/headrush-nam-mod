import QtQuick 2.2

// The picture on the left of a pedal's detail page (288 x 457), drawn instead of the generic "Other.png" the stock page
// falls back to for pedals without an image. NAM and Reverb get their own pedal whose knobs turn with the live values;
// anything else keeps the stock fallback picture.
Item {
	id: root
	width: 288
	height: 457

	property var cm: Evil.getProperty("/Engine/RedirCtrl/CurModule")
	property var tp: cm && cm.translator ? Evil.getProperty("/Engine/Patch/Chain/ModuleType" + cm.translator.index) : null
	property string pedal: tp && tp.translator ? tp.translator.string : ""
	property bool isNam: pedal.indexOf("NAM") === 0
	property bool isReverb: pedal.indexOf("Reverb") === 0
	property bool custom: isNam || isReverb

	function prop(n) { return pedal !== "" ? Evil.getProperty("/Engine/Patch/" + pedal + "/" + n) : null }
	function norm(pr) { return pr && pr.translator ? Math.max(0, Math.min(1, pr.translator.value)) : 0.5 }

	property var driveP: prop("Drive")
	property var levelP: prop("Level")
	property var toneP: prop("Tone")
	property var onP: prop("On")
	property var gainP: prop("Gain")
	property var hicutP: prop("Hi-Cut")
	property bool active: onP && onP.translator ? onP.translator.state : true

	property var kx: [78, 210, 144]
	property var ky: [112, 112, 210]
	property var kv: [norm(driveP), norm(levelP), norm(toneP)]
	property var kl: isReverb ? ["DECAY", "MIX", "DAMPING"] : ["MODEL", "INPUT", "OUTPUT"]

	property color accent: isReverb ? "#3fd0ff" : "#b5e61d"
	property string title: isReverb ? "REVERB" : "NAM"
	property string subtitle: isReverb ? algoName : "NEURAL AMP MODELER"

	// Reverb: the algorithm, from the same two switches the hook reads (see reverb_ui.qml)
	property int algo: gainP && hicutP && gainP.translator && hicutP.translator
		? (hicutP.translator.index === 0 ? (gainP.translator.index === 1 ? 3 : 2) : (gainP.translator.index === 1 ? 1 : 0)) : 0
	property string algoName: ["PLATE", "HALL", "ROOM", "SPRING"][algo]

	// NAM: the model name next to the number, from the same list the page uses
	property double stamp: new Date().getTime()
	Loader { id: modelList; source: root.isNam ? "file:///tmp/nam_models.qml?" + root.stamp : "" }
	property var names: modelList.item ? modelList.item.names : []
	property int modelIdx: driveP && driveP.translator && names.length > 0
		? Math.max(0, Math.min(names.length - 1, Math.round(driveP.translator.unnormalized) - 1)) : -1
	property string modelName: modelIdx >= 0 ? names[modelIdx] : ""

	// the stock picture for everything else
	Image { anchors.fill: parent; source: "qrc:/Pages/ModuleEditors/img/Other.png"; visible: !root.custom; fillMode: Image.PreserveAspectFit }

	Item {
		anchors.fill: parent
		visible: root.custom

		// body
		Rectangle {
			x: 14; y: 6; width: 260; height: 445; radius: 20
			border.width: 3; border.color: root.active ? root.accent : "#4a4b4d"
			gradient: Gradient {
				GradientStop { position: 0.0; color: "#34363a" }
				GradientStop { position: 0.5; color: "#1f2023" }
				GradientStop { position: 1.0; color: "#141517" }
			}
		}
		// screws
		Repeater {
			model: [[28, 20], [260, 20], [28, 437], [260, 437]]
			Rectangle { x: modelData[0] - 5; y: modelData[1] - 5; width: 10; height: 10; radius: 5; color: "#55575b"; border.width: 1; border.color: "#222" }
		}
		// LED
		Rectangle {
			x: 144 - 7; y: 24; width: 14; height: 14; radius: 7
			color: root.active ? root.accent : "#2a2b2d"; border.width: 2; border.color: "#0c0c0d"
		}

		// knobs: the Repeater only counts, the values live in properties so the knobs update in place
		Repeater {
			model: 3
			Item {
				x: root.kx[index] - 36; y: root.ky[index] - 36; width: 72; height: 72
				Rectangle {
					anchors.fill: parent; radius: 36; border.width: 2; border.color: "#0b0b0c"
					gradient: Gradient {
						GradientStop { position: 0.0; color: "#4b4d52" }
						GradientStop { position: 1.0; color: "#17181a" }
					}
				}
				Rectangle { x: 10; y: 10; width: 52; height: 52; radius: 26; color: "#101113"; border.width: 1; border.color: "#2d2f33" }
				Item {
					anchors.fill: parent
					rotation: -135 + 270 * root.kv[index]
					Rectangle { x: 33; y: 6; width: 6; height: 26; radius: 3; color: root.accent }
				}
				Text {
					y: 76; width: 72; horizontalAlignment: Text.AlignHCenter
					text: root.kl[index]; color: "#d6d8dc"; font.pixelSize: 17; font.bold: true; font.letterSpacing: 1
				}
			}
		}

		// name plate
		Rectangle { x: 34; y: 296; width: 220; height: 2; color: root.accent; opacity: 0.8 }
		Text {
			x: 14; y: 304; width: 260; horizontalAlignment: Text.AlignHCenter
			text: root.title; color: "white"; font.pixelSize: 52; font.bold: true; font.letterSpacing: 6
		}
		Text {
			x: 14; y: 362; width: 260; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight
			text: root.subtitle; color: root.accent; font.pixelSize: 15; font.bold: true; font.letterSpacing: 2
		}
		// NAM: the loaded model
		Text {
			x: 24; y: 384; width: 240; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight
			visible: root.isNam && root.modelName !== ""
			text: (root.modelIdx + 1) + "  " + root.modelName; color: "#d6d8dc"; font.pixelSize: 18
		}

		// footswitch
		Rectangle {
			x: 144 - 24; y: 408; width: 48; height: 36; radius: 18; border.width: 2; border.color: "#0b0b0c"
			gradient: Gradient {
				GradientStop { position: 0.0; color: "#c9ccd1" }
				GradientStop { position: 1.0; color: "#6b6e74" }
			}
		}
	}
}
