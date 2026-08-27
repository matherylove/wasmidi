import QtQuick
import QtQuick.Layouts

Item {
    id: root
    required property var mainWindow
    implicitHeight: 142

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        Text {
            text: "FILE INFO"
            color: "#6f6685"
            font.pixelSize: 9
            font.bold: true
            font.letterSpacing: 1.1
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: 3
            rowSpacing: 4
            columnSpacing: 4

            Repeater {
                model: [
                    ["Notes", formatInteger(root.mainWindow.noteCount)],
                    ["Tracks", formatInteger(root.mainWindow.trackCount)],
                    ["Dur", formatTime(root.mainWindow.duration)],
                    ["BPM", formatNumber(root.mainWindow.bpm, 2)],
                    ["Chs", formatInteger(root.mainWindow.activeChannelCount)],
                    ["Fmt", formatInteger(root.mainWindow.midiFormat)],
                    ["PPQ", formatInteger(root.mainWindow.ppq)],
                    ["Tmp Chg", formatInteger(root.mainWindow.tempoChangeCount)],
                    ["Pk NPS", formatInteger(root.mainWindow.peakNps)],
                    ["Pk Poly", formatInteger(root.mainWindow.peakPolyphony)],
                    ["CC Evts", formatInteger(root.mainWindow.controlEventCount)],
                    ["Pitch", root.mainWindow.pitchRange]
                ]

                delegate: Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 4
                    color: "#0d0b1c"
                    border.color: "#1d172e"
                    border.width: 1
                    Column {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.topMargin: 3
                        spacing: 0
                        Text { text: modelData[0].toUpperCase(); color: "#554d66"; font.pixelSize: 6; font.bold: true; font.letterSpacing: 0.4 }
                        Text {
                            text: modelData[1]
                            color: "#bdb0d5"
                            font.pixelSize: 9
                            minimumPixelSize: 6
                            fontSizeMode: Text.HorizontalFit
                            font.bold: true
                            wrapMode: Text.NoWrap
                            width: parent.width - 8
                        }
                    }
                }
            }
        }
    }

    function groupIntegerString(digits) {
        var out = ""
        for (var i = 0; i < digits.length; ++i) {
            if (i > 0 && ((digits.length - i) % 3) === 0)
                out += ","
            out += digits.charAt(i)
        }
        return out
    }

    function formatInteger(value) {
        var n = Number(value)
        if (!isFinite(n))
            return "—"
        var rounded = Math.round(n)
        var negative = rounded < 0
        var digits = Math.abs(rounded).toFixed(0)
        return (negative ? "-" : "") + groupIntegerString(digits)
    }

    function formatNumber(value, maxDecimals) {
        var n = Number(value)
        if (!isFinite(n))
            return "—"
        var decimals = Math.max(0, Math.min(8, Math.floor(Number(maxDecimals) || 0)))
        var negative = n < 0
        var fixed = Math.abs(n).toFixed(decimals)
        var dot = fixed.indexOf(".")
        var integerPart = dot >= 0 ? fixed.substring(0, dot) : fixed
        var decimalPart = dot >= 0 ? fixed.substring(dot + 1) : ""
        while (decimalPart.length > 0 && decimalPart.charAt(decimalPart.length - 1) === "0")
            decimalPart = decimalPart.substring(0, decimalPart.length - 1)
        return (negative ? "-" : "") + groupIntegerString(integerPart) +
            (decimalPart.length > 0 ? "." + decimalPart : "")
    }

    function formatTime(seconds) {
        var s = Math.max(0, Math.floor(seconds))
        var h = Math.floor(s / 3600)
        var m = Math.floor((s % 3600) / 60)
        var r = s % 60
        if (h > 0)
            return h + ":" + (m < 10 ? "0" : "") + m + ":" + (r < 10 ? "0" : "") + r
        return m + ":" + (r < 10 ? "0" : "") + r
    }
}
