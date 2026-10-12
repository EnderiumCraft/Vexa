//! The address bar's text and its editing: a cursor, a selection, and the usual keys.

use crate::ffi;

#[derive(Default)]
pub struct Address {
    /// What the bar shows: the page's address, or what is being typed.
    pub text: String,
    pub focused: bool,
    /// The cursor, in bytes into `text`.
    pub cursor: usize,
    /// The other end of the selection, in bytes, when there is one.
    pub anchor: Option<usize>,
}

pub enum Key<'a> {
    Char(char),
    Backspace,
    Delete,
    Left,
    Right,
    Home,
    End,
    SelectAll,
    Paste(&'a str),
}

impl Address {
    pub fn set(&mut self, text: &str) {
        self.text = text.to_string();
        self.cursor = self.text.len();
        self.anchor = None;
    }

    /// Takes the keyboard, with all the text selected.
    pub fn focus(&mut self) {
        self.focused = true;
        self.anchor = Some(0);
        self.cursor = self.text.len();
    }

    pub fn unfocus(&mut self) {
        self.focused = false;
        self.anchor = None;
    }

    pub fn selection(&self) -> Option<(usize, usize)> {
        let anchor = self.anchor?;
        (anchor != self.cursor).then(|| (anchor.min(self.cursor), anchor.max(self.cursor)))
    }

    pub fn selected_text(&self) -> Option<&str> {
        self.selection().map(|(from, to)| &self.text[from..to])
    }

    fn delete_selection(&mut self) -> bool {
        match self.selection() {
            Some((from, to)) => {
                self.text.replace_range(from..to, "");
                self.cursor = from;
                self.anchor = None;
                true
            },
            None => {
                self.anchor = None;
                false
            },
        }
    }

    fn previous(&self, at: usize) -> usize {
        self.text[..at].char_indices().next_back().map_or(0, |(i, _)| i)
    }

    fn next(&self, at: usize) -> usize {
        self.text[at..].chars().next().map_or(at, |c| at + c.len_utf8())
    }

    /// Applies a key. `select`: Shift is down, so movement extends the selection.
    pub fn key(&mut self, key: Key, select: bool) {
        match key {
            Key::Char(c) => {
                self.delete_selection();
                self.text.insert(self.cursor, c);
                self.cursor += c.len_utf8();
            },
            Key::Paste(text) => {
                self.delete_selection();
                let clean: String = text.chars().filter(|c| !c.is_control()).collect();
                self.text.insert_str(self.cursor, &clean);
                self.cursor += clean.len();
            },
            Key::Backspace => {
                if !self.delete_selection() && self.cursor > 0 {
                    let from = self.previous(self.cursor);
                    self.text.replace_range(from..self.cursor, "");
                    self.cursor = from;
                }
            },
            Key::Delete => {
                if !self.delete_selection() && self.cursor < self.text.len() {
                    let to = self.next(self.cursor);
                    self.text.replace_range(self.cursor..to, "");
                }
            },
            Key::Left | Key::Right | Key::Home | Key::End => {
                let target = match key {
                    Key::Left => match (self.selection(), select) {
                        (Some((from, _)), false) => from,
                        _ => self.previous(self.cursor),
                    },
                    Key::Right => match (self.selection(), select) {
                        (Some((_, to)), false) => to,
                        _ => self.next(self.cursor),
                    },
                    Key::Home => 0,
                    _ => self.text.len(),
                };
                if select {
                    self.anchor.get_or_insert(self.cursor);
                } else {
                    self.anchor = None;
                }
                self.cursor = target;
            },
            Key::SelectAll => {
                self.anchor = Some(0);
                self.cursor = self.text.len();
            },
        }
    }

    /// The byte of the text nearest to a click `x` pixels from where the visible text starts.
    pub fn byte_at(&self, start: usize, x: i32) -> usize {
        let visible = &self.text[start..];
        let mut best = start;
        let mut end = ffi::fit_bytes(visible, x.max(0));
        // The character that the click is half way into is the one it lands after.
        if end < visible.len() {
            let next = visible[end..].chars().next().map_or(0, |c| c.len_utf8());
            let before = ffi::text_width(&visible[..end]);
            let after = ffi::text_width(&visible[..end + next]);
            if x - before > after - x {
                end += next;
            }
        }
        best += end;
        best
    }

    /// Where the visible part of the text starts, so that the cursor is in a field `width`
    /// pixels wide: the text is cut from its beginning until it is.
    pub fn visible_start(&self, width: i32) -> usize {
        let mut start = 0;
        while start < self.cursor && ffi::text_width(&self.text[start..self.cursor]) > width - 4 {
            start = self.next(start);
        }
        start
    }
}
