/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{js,ts,jsx,tsx}'],
  theme: {
    extend: {
      fontFamily: {
        sans: ['"DM Sans"', 'system-ui', 'sans-serif'],
        mono: ['"JetBrains Mono"', '"Courier New"', 'monospace'],
      },
      colors: {
        stone: {
          925: '#1C1917',
        },
        walnut: {
          50:  '#FFFBEB',
          100: '#FEF3C7',
          200: '#FCD34D',
          300: '#FBBF24',
          400: '#F59E0B',
          500: '#D97706',
          600: '#B45309',
          700: '#92400E',
          800: '#78350F',
          900: '#451A03',
        },
      },
      animation: {
        'bid-flash': 'bid-flash 600ms ease',
      },
      keyframes: {
        'bid-flash': {
          '0%':   { borderColor: '#059669' },
          '100%': { borderColor: '#3D3835' },
        },
      },
    },
  },
  plugins: [],
};
