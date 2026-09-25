// Reveal-on-scroll: adds `.is-visible` to `.mirage-reveal` elements as they
// enter the viewport. Motion itself is gated by `prefers-reduced-motion` in
// CSS, so this only toggles a class — safe for reduced-motion users.
function initReveal(): void {
  const items = document.querySelectorAll<HTMLElement>('.mirage-reveal');
  if (items.length === 0) return;

  // Fallback: if IntersectionObserver is missing, just show everything.
  if (!('IntersectionObserver' in window)) {
    items.forEach((el) => el.classList.add('is-visible'));
    return;
  }

  const observer = new IntersectionObserver(
    (entries) => {
      for (const entry of entries) {
        if (entry.isIntersecting) {
          const el = entry.target as HTMLElement;
          const delay = el.dataset.delay;
          if (delay) el.style.animationDelay = `${delay}ms`;
          el.classList.add('is-visible');
          observer.unobserve(el);
        }
      }
    },
    { rootMargin: '0px 0px -10% 0px', threshold: 0.1 },
  );

  items.forEach((el) => observer.observe(el));
}

if (document.readyState !== 'loading') {
  initReveal();
} else {
  document.addEventListener('DOMContentLoaded', initReveal);
}

// Re-run after Astro client-side navigation (view transitions).
document.addEventListener('astro:page-load', initReveal);
