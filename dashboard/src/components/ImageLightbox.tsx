import { useState, useEffect } from 'react';

interface Props {
  images: string[];
  initialIndex?: number;
  onClose: () => void;
  title?: string;
}

export function ImageLightbox({ images, initialIndex = 0, onClose, title }: Props) {
  const [idx, setIdx] = useState(initialIndex);

  useEffect(() => {
    function onKeyDown(e: KeyboardEvent) {
      if (e.key === 'Escape') onClose();
      if (e.key === 'ArrowRight') setIdx((i) => (i + 1) % images.length);
      if (e.key === 'ArrowLeft') setIdx((i) => (i - 1 + images.length) % images.length);
    }
    window.addEventListener('keydown', onKeyDown);
    document.body.style.overflow = 'hidden';
    return () => {
      window.removeEventListener('keydown', onKeyDown);
      document.body.style.overflow = '';
    };
  }, [images.length, onClose]);

  if (!images.length) return null;

  return (
    <div
      className="lightbox-overlay"
      role="dialog"
      aria-modal="true"
      aria-label="Image preview"
      onClick={(e) => {
        if (e.target === e.currentTarget) onClose();
      }}
    >
      <div className="lightbox-topbar">
        <div className="lightbox-title">{title || 'Photo view'}</div>
        <div className="lightbox-counter">
          {idx + 1} / {images.length}
        </div>
        <button className="lightbox-close btn" onClick={onClose} aria-label="Close lightbox">
          ✕
        </button>
      </div>

      <div className="lightbox-stage">
        {images.length > 1 && (
          <button
            className="lightbox-nav-btn prev"
            onClick={(e) => {
              e.stopPropagation();
              setIdx((i) => (i - 1 + images.length) % images.length);
            }}
            aria-label="Previous image"
          >
            ‹
          </button>
        )}

        <div className="lightbox-img-wrap" onClick={onClose}>
          <img
            src={images[idx]}
            alt={title ? `${title} enlarged` : 'Enlarged view'}
            className="lightbox-img"
            onClick={(e) => e.stopPropagation()}
            onError={(e) => {
              (e.currentTarget as HTMLImageElement).src = '/placeholder.svg';
            }}
          />
        </div>

        {images.length > 1 && (
          <button
            className="lightbox-nav-btn next"
            onClick={(e) => {
              e.stopPropagation();
              setIdx((i) => (i + 1) % images.length);
            }}
            aria-label="Next image"
          >
            ›
          </button>
        )}
      </div>

      {images.length > 1 && (
        <div className="lightbox-thumbs">
          {images.map((src, i) => (
            <button
              key={src}
              className={`lightbox-thumb${i === idx ? ' active' : ''}`}
              onClick={(e) => {
                e.stopPropagation();
                setIdx(i);
              }}
              aria-label={`Jump to image ${i + 1}`}
            >
              <img src={src} alt="" />
            </button>
          ))}
        </div>
      )}
    </div>
  );
}
