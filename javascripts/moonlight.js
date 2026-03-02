// MoonLight Homepage JavaScript Enhancement - Clean Version
document.addEventListener('DOMContentLoaded', function() {
    // Initialize all features
    createScrollProgress();
    setupScrollAnimations();
    setupCounterAnimations();
    createFloatingElements();
    setupInteractiveElements();
    setupParallaxEffect();
    setupThemeColorChange();
    setupSparkleEffects();
    addRequiredStyles();
});

// Create Scroll Progress Bar
function createScrollProgress() {
    const progressBar = document.createElement('div');
    progressBar.className = 'scroll-progress';
    document.body.appendChild(progressBar);
    
    window.addEventListener('scroll', () => {
        const scrollPercent = (window.scrollY / (document.documentElement.scrollHeight - window.innerHeight)) * 100;
        progressBar.style.width = scrollPercent + '%';
    });
}

// Setup Scroll Animations (Consolidated)
function setupScrollAnimations() {
    const observerOptions = {
        threshold: 0.1,
        rootMargin: '0px 0px -50px 0px'
    };
    
    const observer = new IntersectionObserver((entries) => {
        entries.forEach((entry, index) => {
            if (entry.isIntersecting) {
                // Add staggered delay for multiple elements
                setTimeout(() => {
                    entry.target.classList.add('animate-in');
                    entry.target.classList.add('animate');
                }, index * 100);
                observer.unobserve(entry.target);
            }
        });
    }, observerOptions);
    
    // Observe all animation elements
    const animateElements = document.querySelectorAll(
        '.moonlight-stats, .features-section, .performance-section, ' +
        '.feature-card, .moonlight-stat, .moonlight-buttons, ' +
        '.slide-in-left, .slide-in-right, .scale-in, .scroll-animate'
    );
    
    animateElements.forEach(el => observer.observe(el));
}

// Animated Counters for Stats
function setupCounterAnimations() {
    const stats = document.querySelectorAll('.moonlight-stat-number');
    const statsObserver = new IntersectionObserver((entries) => {
        entries.forEach(entry => {
            if (entry.isIntersecting) {
                animateCounter(entry.target);
                statsObserver.unobserve(entry.target);
            }
        });
    }, { threshold: 0.5 });
    
    stats.forEach(stat => statsObserver.observe(stat));
}

function animateCounter(element) {
    const text = element.textContent;
    const number = parseInt(text.replace(/\D/g, ''));
    const suffix = text.replace(/[\d,]/g, '');
    
    if (isNaN(number)) return;
    
    const duration = 2000;
    const steps = 60;
    const increment = number / steps;
    const stepTime = duration / steps;
    
    let current = 0;
    const timer = setInterval(() => {
        current += increment;
        if (current >= number) {
            current = number;
            clearInterval(timer);
        }
        
        if (number >= 1000) {
            element.textContent = Math.floor(current).toLocaleString() + suffix;
        } else {
            element.textContent = Math.floor(current) + suffix;
        }
    }, stepTime);
}

// Create Floating Elements
function createFloatingElements() {
    const hero = document.querySelector('.moonlight-hero');
    if (!hero || window.innerWidth <= 768) return; // Skip on mobile
    
    const emojis = ['🌙', '⭐', '✨'];
    
    for (let i = 0; i < 3; i++) {
        const floating = document.createElement('div');
        floating.className = 'floating-element';
        floating.innerHTML = emojis[i];
        floating.style.cssText = `
            top: ${Math.random() * 70 + 10}%;
            left: ${Math.random() * 80 + 10}%;
            font-size: ${Math.random() * 20 + 20}px;
            animation-duration: ${Math.random() * 4 + 4}s;
        `;
        hero.appendChild(floating);
    }
}

// Interactive Elements
function setupInteractiveElements() {
    // Feature cards tilt effect (desktop only)
    if (window.innerWidth > 768) {
        const cards = document.querySelectorAll('.feature-card, .moonlight-stat');
        cards.forEach(card => {
            card.addEventListener('mousemove', handleCardTilt);
            card.addEventListener('mouseleave', resetCardTilt);
        });
    }
    
    // Button ripple effect
    const buttons = document.querySelectorAll('.moonlight-btn');
    buttons.forEach(button => {
        button.addEventListener('click', createRipple);
    });
    
    // Stats card click effect
    const statCards = document.querySelectorAll('.moonlight-stat');
    statCards.forEach(card => {
        card.addEventListener('click', function() {
            this.style.transform = 'scale(0.95)';
            setTimeout(() => {
                this.style.transform = '';
            }, 150);
        });
    });
}

function handleCardTilt(e) {
    const card = e.currentTarget;
    const rect = card.getBoundingClientRect();
    const centerX = rect.left + rect.width / 2;
    const centerY = rect.top + rect.height / 2;
    const deltaX = (e.clientX - centerX) / (rect.width / 2);
    const deltaY = (e.clientY - centerY) / (rect.height / 2);
    
    card.style.transform = `
        perspective(1000px) 
        rotateX(${deltaY * -5}deg) 
        rotateY(${deltaX * 5}deg) 
        translateY(-10px)
    `;
}

function resetCardTilt(e) {
    e.currentTarget.style.transform = '';
}

function createRipple(e) {
    const button = e.currentTarget;
    const ripple = document.createElement('span');
    const rect = button.getBoundingClientRect();
    const size = Math.max(rect.width, rect.height);
    const x = e.clientX - rect.left - size / 2;
    const y = e.clientY - rect.top - size / 2;
    
    ripple.style.cssText = `
        position: absolute;
        width: ${size}px;
        height: ${size}px;
        left: ${x}px;
        top: ${y}px;
        background: rgba(255, 255, 255, 0.3);
        border-radius: 50%;
        transform: scale(0);
        animation: ripple 0.6s linear;
        pointer-events: none;
    `;
    
    button.style.position = 'relative';
    button.style.overflow = 'hidden';
    button.appendChild(ripple);
    
    ripple.addEventListener('animationend', () => ripple.remove());
}

// Parallax Effect (desktop only)
function setupParallaxEffect() {
    if (window.innerWidth <= 768) return; // Skip on mobile for performance
    
    let ticking = false;
    
    function updateParallax() {
        const scrolled = window.pageYOffset;
        const hero = document.querySelector('.moonlight-hero');
        const performance = document.querySelector('.performance-section');
        
        if (hero) {
            hero.style.transform = `translateY(${scrolled * 0.3}px)`;
        }
        
        if (performance) {
            performance.style.transform = `translateY(${scrolled * 0.1}px)`;
        }
        
        ticking = false;
    }
    
    window.addEventListener('scroll', () => {
        if (!ticking) {
            requestAnimationFrame(updateParallax);
            ticking = true;
        }
    });
}

// Dynamic theme color changes based on scroll
function setupThemeColorChange() {
    let ticking = false;
    
    function updateThemeColor() {
        const scrollPercent = window.scrollY / (document.documentElement.scrollHeight - window.innerHeight);
        const hue = 250 + (scrollPercent * 30); // From purple to pink
        document.documentElement.style.setProperty('--moonlight-primary', `hsl(${hue}, 60%, 70%)`);
        ticking = false;
    }
    
    window.addEventListener('scroll', () => {
        if (!ticking) {
            requestAnimationFrame(updateThemeColor);
            ticking = true;
        }
    });
}

// Sparkle effects on click
function setupSparkleEffects() {
    document.addEventListener('click', (e) => {
        if (Math.random() > 0.8) { // Reduced frequency
            createSparkle(e.clientX, e.clientY);
        }
    });
}

function createSparkle(x, y) {
    const sparkle = document.createElement('div');
    sparkle.innerHTML = '✨';
    sparkle.style.cssText = `
        position: fixed;
        left: ${x}px;
        top: ${y}px;
        font-size: 16px;
        pointer-events: none;
        z-index: 9999;
        animation: sparkleAnim 1s ease-out forwards;
    `;
    
    document.body.appendChild(sparkle);
    setTimeout(() => sparkle.remove(), 1000);
}

// Add required CSS animations
function addRequiredStyles() {
    const styles = document.createElement('style');
    styles.textContent = `
        @keyframes ripple {
            to {
                transform: scale(4);
                opacity: 0;
            }
        }
        
        @keyframes sparkleAnim {
            0% {
                opacity: 1;
                transform: scale(0) rotate(0deg);
            }
            100% {
                opacity: 0;
                transform: scale(1.5) rotate(180deg) translateY(-50px);
            }
        }
    `;
    document.head.appendChild(styles);
}