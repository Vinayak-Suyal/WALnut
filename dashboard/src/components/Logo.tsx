// WALnut Geometric Logo Kit Components
// Implements SVG icons and complete responsive wordmark from WALnut-logo-kit

export function WalnutGeometricIcon({ size = 26, className = '' }: { size?: number; className?: string }) {
  return (
    <svg
      xmlns="http://www.w3.org/2000/svg"
      viewBox="0 0 96 96"
      width={size}
      height={size}
      className={className}
      style={{ display: 'inline-block', flexShrink: 0, verticalAlign: 'middle' }}
      role="img"
      aria-label="WALnut geometric logo"
    >
      <path
        fill="#C78B2C"
        fillRule="evenodd"
        d="M48 8L69 19L80 39L76 64L61 81L48 88L35 81L20 64L16 39L27 19ZM27 35L37 70L48 52L59 70L69 35L61 35L56 52L48 39L40 52L35 35Z"
      />
    </svg>
  );
}

// Complete WALnut logo with golden icon mark and wordmark text
// Text fill automatically uses var(--text) for seamless dark/light mode adaptation
export function WalnutFullLogo({ height = 32, className = '' }: { height?: number; className?: string }) {
  return (
    <svg
      xmlns="http://www.w3.org/2000/svg"
      viewBox="0 0 246 64"
      height={height}
      className={className}
      style={{
        height,
        width: 'auto',
        display: 'inline-block',
        verticalAlign: 'middle',
      }}
      role="img"
      aria-label="WALnut"
    >
      <svg width="64" height="64" viewBox="0 0 96 96" role="img" aria-label="WALnut mark">
        <path
          fill="#C78B2C"
          fillRule="evenodd"
          d="M48 8L69 19L80 39L76 64L61 81L48 88L35 81L20 64L16 39L27 19ZM27 35L37 70L48 52L59 70L69 35L61 35L56 52L48 39L40 52L35 35Z"
        />
      </svg>
      <text
        x="78"
        y="43"
        fontFamily="Inter, 'Segoe UI', Arial, sans-serif"
        fontSize="34"
        fontWeight="700"
        letterSpacing="-1"
        fill="var(--text)"
      >
        WALnut
      </text>
    </svg>
  );
}
