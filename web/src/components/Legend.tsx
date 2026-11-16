const ENTRIES = [
  { className: 'premium-tw', label: '3W', text: 'Triple word' },
  { className: 'premium-dw', label: '2W', text: 'Double word' },
  { className: 'premium-tl', label: '3L', text: 'Triple letter' },
  { className: 'premium-dl', label: '2L', text: 'Double letter' },
];

/** Key for the premium-square colours, plus the tile conventions. */
export function Legend() {
  return (
    <ul className="legend" aria-label="Legend">
      {ENTRIES.map((entry) => (
        <li key={entry.label}>
          <span className={`legend-swatch ${entry.className}`}>{entry.label}</span>
          {entry.text}
        </li>
      ))}
      <li>
        <span className="legend-swatch legend-tile is-blank">a</span>
        Blank
      </li>
      <li>
        <span className="legend-swatch legend-tile is-preview">A</span>
        Preview
      </li>
    </ul>
  );
}
