// static/script.js
let currentUser = null; // [cite: 268]

// Cargar videos al inicio
document.addEventListener('DOMContentLoaded', function() {
    loadVideos();
    // Check if token is present on page load
    const savedToken = localStorage.getItem('token'); // [cite: 304]
    if (savedToken) {
        // In a real app, you'd send this token to your server to validate and get user info
        // For now, we'll assume it means the user is "logged in" for basic UI changes
        // A more robust check: fetch('/api/user/me', { headers: { 'Authorization': 'Bearer ' + savedToken }})
        currentUser = { token: savedToken }; // Placeholder user info
        updateNavigation();
    }
}); // [cite: 269]

function showAuth() {
    document.getElementById('auth-modal').classList.remove('hidden'); // [cite: 270]
}

function hideAuth() {
    document.getElementById('auth-modal').classList.add('hidden'); // [cite: 271]
}

function showUpload() {
    if(!currentUser) {
        alert('Debes iniciar sesión para subir videos'); // [cite: 272]
        showAuth();
        return;
    }
    document.getElementById('upload-modal').classList.remove('hidden'); // [cite: 273]
}

function hideUpload() {
    document.getElementById('upload-modal').classList.add('hidden'); // [cite: 274]
}

function updateFileName(input) {
    const fileName = input.files[0]?.name || 'Ningún archivo seleccionado'; // [cite: 275]
    document.getElementById('file-name').textContent = fileName;
}

// Manejar login
document.getElementById('login-form').addEventListener('submit', async function(e) {
    e.preventDefault();
    const username = document.getElementById('login-username').value;
    const password = document.getElementById('login-password').value;

    try {
        const response = await fetch('/api/login', { // [cite: 276]
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: `username=${encodeURIComponent(username)}&password=${encodeURIComponent(password)}`
        });

        const result = await response.json(); // [cite: 277]
        if(result.success) {
            currentUser = result.user;
            localStorage.setItem('token', result.token);
            alert('Sesión iniciada correctamente'); // [cite: 278]
            hideAuth();
            updateNavigation();
        } else {
            alert('Error: ' + result.message); // [cite: 279]
        }
    } catch(error) {
        alert('Error de conexión'); // [cite: 280]
        console.error('Login error:', error);
    }
});

// Manejar registro
document.getElementById('register-form').addEventListener('submit', async function(e) {
    e.preventDefault();
    const username = document.getElementById('register-username').value;
    const email = document.getElementById('register-email').value;
    const password = document.getElementById('register-password').value;
    const role = document.getElementById('register-role').value;

    try { // [cite: 281]
        const response = await fetch('/api/register', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: `username=${encodeURIComponent(username)}&email=${encodeURIComponent(email)}&password=${encodeURIComponent(password)}&role=${encodeURIComponent(role)}`
        }); // [cite: 282]

        const result = await response.json();
        if(result.success) {
            alert('Usuario registrado correctamente. Ahora puedes iniciar sesión.'); // [cite: 283]
            document.getElementById('register-form').reset();
        } else {
            alert('Error: ' + result.message); // [cite: 284]
        }
    } catch(error) {
        alert('Error de conexión'); // [cite: 285]
        console.error('Register error:', error);
    }
});

// Manejar subida de videos
document.getElementById('upload-form').addEventListener('submit', async function(e) {
    e.preventDefault();
    const formData = new FormData();
    
    formData.append('title', document.getElementById('video-title').value);
    formData.append('description', document.getElementById('video-description').value);
    formData.append('category', document.getElementById('video-category').value); // [cite: 286]
    formData.append('tags', document.getElementById('video-tags').value);
    formData.append('video', document.getElementById('video-file').files[0]);

    try {
        const response = await fetch('/api/upload', {
            method: 'POST',
            headers: { 'Authorization': 'Bearer ' + localStorage.getItem('token') }, // [cite: 287]
            body: formData
        });

        const result = await response.json();
        if(result.success) { // [cite: 288]
            alert('Video subido correctamente');
            hideUpload();
            loadVideos();
            document.getElementById('upload-form').reset(); // [cite: 289]
        } else {
            alert('Error: ' + result.message); // [cite: 290]
        }
    } catch(error) {
        alert('Error de conexión'); // [cite: 291]
        console.error('Upload error:', error);
    }
});

// Cargar lista de videos
async function loadVideos() {
    try {
        const response = await fetch('/api/videos'); // [cite: 292]
        const result = await response.json();
        
        if(result.success) {
            displayVideos(result.videos); // [cite: 293]
        }
    } catch(error) {
        console.error('Error cargando videos:', error); // [cite: 294]
    }
}

function displayVideos(videos) {
    const container = document.getElementById('video-list'); // [cite: 295]
    container.innerHTML = '';

    videos.forEach(video => {
        const videoCard = document.createElement('div');
        videoCard.className = 'video-card';
        videoCard.innerHTML = `
            <div class="video-thumbnail">
                <div style="color: #666;">▶️ Video</div> </div>
            <div class="video-info">
                <div class="video-title">${escapeHtml(video.title)}</div>
                <div class="video-meta">
                    👀 ${video.views} visualizaciones • ❤️ ${video.likes} likes • 
                    📁 ${video.category}
                </div> <p style="margin-top: 0.5rem; color: #666;">${escapeHtml(video.description.substring(0, 100))}...</p>
                <button onclick="watchVideo(${video.id})" class="btn" style="margin-top: 1rem; padding: 8px 16px;">Ver Video</button>
            </div>
        `; // [cite: 299]
        container.appendChild(videoCard);
    }); // [cite: 300]
}

function watchVideo(videoId) {
    window.location.href = `/watch?id=${videoId}`; // [cite: 301]
}

function escapeHtml(text) {
    const div = document.createElement('div'); // [cite: 302]
    div.textContent = text;
    return div.innerHTML;
}

function updateNavigation() {
    // Update navigation based on whether the user is logged in
    const loginLink = document.querySelector('.nav-links a[onclick="showAuth()"]');
    const uploadLink = document.querySelector('.nav-links a[onclick="showUpload()"]');
    
    if (currentUser) {
        if (loginLink) loginLink.textContent = 'Logout';
        if (loginLink) loginLink.onclick = function() {
            localStorage.removeItem('token');
            currentUser = null;
            alert('Sesión cerrada');
            updateNavigation();
            window.location.reload();
        };
        if (uploadLink) uploadLink.style.display = 'block'; // Show upload link
    } else {
        if (loginLink) loginLink.textContent = 'Login';
        if (loginLink) loginLink.onclick = function() { showAuth(); };
        if (uploadLink) uploadLink.style.display = 'none'; // Hide upload link
    }
}
