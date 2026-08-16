const express = require('express');
const path = require('path');
const fs = require('fs');
const compression = require('compression');

const app = express();
const PORT = process.env.PORT || 3000;
const PUBLIC_DIR = __dirname;
const DOWNLOADS_DIR = path.join(__dirname, 'downloads');

// Enable Gzip compression for faster web delivery
app.use(compression());

// Security & Caching headers
app.use((req, res, next) => {
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('X-Frame-Options', 'SAMEORIGIN');
  next();
});

// Dedicated Binary Download Endpoint with proper attachment headers
app.get('/downloads/:filename', (req, res) => {
  const filename = path.basename(req.params.filename);
  const filePath = path.join(DOWNLOADS_DIR, filename);

  if (!fs.existsSync(filePath)) {
    return res.status(404).send('Download file not found');
  }

  // Set appropriate MIME types
  if (filename.endsWith('.apk')) {
    res.setHeader('Content-Type', 'application/vnd.android.package-archive');
  } else if (filename.endsWith('.exe')) {
    res.setHeader('Content-Type', 'application/vnd.microsoft.portable-executable');
  } else {
    res.setHeader('Content-Type', 'application/octet-stream');
  }

  res.setHeader('Content-Disposition', `attachment; filename="${filename}"`);
  
  // Stream file with resumable download support
  const fileStream = fs.createReadStream(filePath);
  fileStream.pipe(res);
});

// Serve Static Assets (HTML, CSS, JS, Images, Favicon)
app.use(express.static(PUBLIC_DIR, {
  maxAge: '1d',
  index: 'index.html'
}));

// Fallback to index.html for SPA routes
app.get('*', (req, res) => {
  res.sendFile(path.join(PUBLIC_DIR, 'index.html'));
});

app.listen(PORT, '0.0.0.0', () => {
  console.log(`========================================`);
  console.log(` NullWire Web & Download Server Active `);
  console.log(` Port: ${PORT}`);
  console.log(` URL:  http://localhost:${PORT}`);
  console.log(` Downloads Directory: ${DOWNLOADS_DIR}`);
  console.log(`========================================`);
});
