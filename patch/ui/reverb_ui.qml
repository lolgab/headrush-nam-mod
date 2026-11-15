import QtQuick 2.2

Column {
	id: root
	width: 720
	spacing: 14
	property var blockPage
	property string pedal: blockPage && blockPage.moduleTypeProp ? blockPage.moduleTypeProp.translator.string : ""
	property var hicut: pedal !== "" ? Evil.getProperty("/Engine/Patch/" + pedal + "/Hi-Cut") : null
	property var gain: pedal !== "" ? Evil.getProperty("/Engine/Patch/" + pedal + "/Gain") : null
	property bool ready: hicut !== null && gain !== null && hicut.translator && gain.translator
	// the hook reads the same two switches: Hi-Cut On: Gain Low = plate / High = hall; Hi-Cut Off: Low = room / High = spring
	property int algo: !ready ? -1 : (hicut.translator.index === 0 ? (gain.translator.index === 1 ? 3 : 2) : (gain.translator.index === 1 ? 1 : 0))
	readonly property var names: ["PLATE", "HALL", "ROOM", "SPRING"]
	readonly property var notes: ["dense, smooth plate", "long, wide hall", "small, natural room", "dripping spring tank"]

	function choose(a) {
		if (!ready)
			return
		hicut.translator.index = a >= 2 ? 0 : 1
		gain.translator.index = (a === 1 || a === 3) ? 1 : 0
	}

	Rectangle {
		width: 720; height: 110; color: "#1b2a18"; border.color: "#44F511"; border.width: 2
		Text { x: 20; y: 8; color: "#9fd99a"; font.pixelSize: 26; text: "ALGORITHM" }
		Text { x: 20; y: 38; color: "white"; font.pixelSize: 44; text: root.algo >= 0 ? root.names[root.algo] : "..." }
		Text { x: 330; y: 58; width: 370; color: "#9fd99a"; font.pixelSize: 24; horizontalAlignment: Text.AlignRight; text: root.algo >= 0 ? root.notes[root.algo] : "" }
	}

	Row {
		spacing: 12
		Repeater {
			model: 4
			Rectangle {
				width: 174; height: 80; radius: 4
				color: root.algo === index ? "#2f6b27" : "#2b2c2e"
				border.width: 2; border.color: root.algo === index ? "#44F511" : "#555"
				Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 30; text: root.names[index] }
				MouseArea { anchors.fill: parent; onClicked: root.choose(index) }
			}
		}
	}

	property var drive: pedal !== "" ? Evil.getProperty("/Engine/Patch/" + pedal + "/Drive") : null
	property var level: pedal !== "" ? Evil.getProperty("/Engine/Patch/" + pedal + "/Level") : null
	property var tone: pedal !== "" ? Evil.getProperty("/Engine/Patch/" + pedal + "/Tone") : null
	property string decayText: drive && drive.translator ? drive.translator.string : ""
	property string mixText: level && level.translator ? level.translator.string : ""
	property string dampText: tone && tone.translator ? tone.translator.string : ""

	// the engine knobs are a Glorious Drive clone's (the stock tiles say DRIVE / LEVEL / TONE): their real meaning here
	Row {
		spacing: 12
		Repeater {
			model: [["DECAY", root.decayText], ["MIX", root.mixText], ["DAMPING", root.dampText]]
			Rectangle {
				width: 232; height: 90; color: "#2b2c2e"; border.width: 2; border.color: "#555"
				Text { x: 14; y: 8; color: "#9fd99a"; font.pixelSize: 24; text: modelData[0] }
				Text { x: 14; y: 40; width: 204; elide: Text.ElideRight; color: "white"; font.pixelSize: 38; text: modelData[1] }
			}
		}
	}
}
