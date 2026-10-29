// BodyRange <-> signal-cli's "start:length:STYLE" strings. Offsets are UTF-16 code units on both sides.

use presage::proto::body_range::{AssociatedValue, Style};
use presage::proto::BodyRange;

fn style_name(style: Style) -> Option<&'static str> {
    match style {
        Style::None => None,
        Style::Bold => Some("BOLD"),
        Style::Italic => Some("ITALIC"),
        Style::Spoiler => Some("SPOILER"),
        Style::Strikethrough => Some("STRIKETHROUGH"),
        Style::Monospace => Some("MONOSPACE"),
    }
}

fn style_from_name(name: &str) -> Option<Style> {
    match name.trim().to_ascii_uppercase().as_str() {
        "BOLD" => Some(Style::Bold),
        "ITALIC" => Some(Style::Italic),
        "SPOILER" => Some(Style::Spoiler),
        "STRIKETHROUGH" => Some(Style::Strikethrough),
        "MONOSPACE" => Some(Style::Monospace),
        _ => None,
    }
}

/// Style ranges of a message as signal-cli strings; mentions are not styles and are skipped.
pub fn to_strings(ranges: &[BodyRange]) -> Vec<String> {
    ranges
        .iter()
        .filter_map(|r| {
            let Some(AssociatedValue::Style(s)) = r.associated_value else { return None };
            let name = style_name(Style::try_from(s).ok()?)?;
            let length = r.length?;
            (length > 0).then(|| format!("{}:{}:{}", r.start.unwrap_or(0), length, name))
        })
        .collect()
}

/// Parses "start:length:STYLE"; malformed entries are an error so the caller learns about them.
pub fn from_strings<'a>(items: impl IntoIterator<Item = &'a str>) -> Result<Vec<BodyRange>, String> {
    items
        .into_iter()
        .map(|item| {
            let parts: Vec<&str> = item.split(':').collect();
            let [start, length, style] = parts[..] else {
                return Err(format!("invalid text style '{item}'"));
            };
            let start: u32 = start.parse().map_err(|_| format!("invalid text style '{item}'"))?;
            let length: u32 = length.parse().map_err(|_| format!("invalid text style '{item}'"))?;
            let style = style_from_name(style).ok_or_else(|| format!("unknown text style '{style}'"))?;
            Ok(BodyRange {
                start: Some(start),
                length: Some(length),
                associated_value: Some(AssociatedValue::Style(style as i32)),
            })
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let ranges = from_strings(["0:4:BOLD", "5:3:italic", "9:2:MONOSPACE"]).unwrap();
        assert_eq!(to_strings(&ranges), ["0:4:BOLD", "5:3:ITALIC", "9:2:MONOSPACE"]);
    }

    #[test]
    fn rejects_malformed() {
        assert!(from_strings(["0:4"]).is_err());
        assert!(from_strings(["a:4:BOLD"]).is_err());
        assert!(from_strings(["0:4:SHOUTING"]).is_err());
    }

    #[test]
    fn skips_mentions_and_empty_ranges() {
        let ranges = vec![
            BodyRange { start: Some(0), length: Some(1), associated_value: Some(AssociatedValue::MentionAci("x".into())) },
            BodyRange { start: Some(2), length: Some(0), associated_value: Some(AssociatedValue::Style(Style::Bold as i32)) },
        ];
        assert!(to_strings(&ranges).is_empty());
    }
}
