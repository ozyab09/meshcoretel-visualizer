const express = require('express');
const WebSocket = require('ws');
const http = require('http');
const path = require('path');
const cors = require('cors');
const helmet = require('helmet');
const axios = require('axios');

const app = express();
const server = http.createServer(app);

// Security middleware
app.use(helmet({
  contentSecurityPolicy: {
    directives: {
      defaultSrc: ["'self'"],
      styleSrc: ["'self'", "'unsafe-inline'"],
      scriptSrc: ["'self'", "'unsafe-inline'", "'unsafe-eval'"],
      imgSrc: ["'self'", "data:", "https://*.tile.openstreetmap.org"],
      connectSrc: ["'self'", "https://www.meshcoretel.ru", "wss://www.meshcoretel.ru"],
      fontSrc: ["'self'"],
      objectSrc: ["'none'"],
      mediaSrc: ["'self'"],
      frameSrc: ["'none'"],
    },
  },
}));
app.use(cors());

// Serve static files
app.use(express.static(path.join(__dirname, 'public')));

// API proxy endpoints
app.get('/api/adverts', async (req, res) => {
  const startedAt = Date.now();
  try {
    const { limit: reqLimit, offset: reqOffset } = req.query;

    // If no limit is specified, fetch all adverts by paginating through all pages
    if (reqLimit === undefined) {
      let allData = [];
      let offset = 0;
      const limit = 100; // Use 100 as the page size for fetching all data

      while(true) {
        const response = await axios.get(`https://www.meshcoretel.ru/api/adverts?limit=${limit}&offset=${offset}`);
        const data = response.data;

        if (!Array.isArray(data) || data.length === 0) {
          break;
        }

        allData = allData.concat(data);

        // If we got less than the limit, it means we're at the last page
        if (data.length < limit) {
          break;
        }

        offset += limit;
      }

      console.log(`GET /api/adverts -> ${allData.length} items in ${Date.now() - startedAt}ms`);
      res.json(allData);
    } else {
      // Use the requested limit and offset
      const limit = parseInt(reqLimit) || 100;
      const offset = parseInt(reqOffset) || 0;
      const response = await axios.get(`https://www.meshcoretel.ru/api/adverts?limit=${limit}&offset=${offset}`);
      const count = Array.isArray(response.data) ? response.data.length : 0;
      console.log(`GET /api/adverts?limit=${limit}&offset=${offset} -> ${count} items in ${Date.now() - startedAt}ms`);
      res.json(response.data);
    }
  } catch (error) {
    console.error('Error fetching adverts:', error.message);
    res.status(500).json({ error: 'Failed to fetch adverts' });
  }
});

app.get('/api/observers', async (req, res) => {
  try {
    const { limit = 100, offset = 0 } = req.query;
    const response = await axios.get(`https://www.meshcoretel.ru/api/observers?limit=${limit}&offset=${offset}`);
    res.json(response.data);
  } catch (error) {
    console.error('Error fetching observers:', error.message);
    res.status(500).json({ error: 'Failed to fetch observers' });
  }
});

app.get('/api/packets', async (req, res) => {
  try {
    const { limit = 100, offset = 0 } = req.query;
    const response = await axios.get(`https://www.meshcoretel.ru/api/packets?limit=${limit}&offset=${offset}`);
    res.json(response.data);
  } catch (error) {
    console.error('Error fetching packets:', error.message);
    res.status(500).json({ error: 'Failed to fetch packets' });
  }
});

app.get('/api/propagations', async (req, res) => {
  try {
    const { limit = 100, offset = 0 } = req.query;
    const response = await axios.get(`https://www.meshcoretel.ru/api/propagations?limit=${limit}&offset=${offset}`);
    res.json(response.data);
  } catch (error) {
    console.error('Error fetching propagations:', error.message);
    res.status(500).json({ error: 'Failed to fetch propagations' });
  }
});

app.get('/api/packets/stats/config', async (req, res) => {
  try {
    const response = await axios.get('https://www.meshcoretel.ru/api/packets/stats/config');
    res.json(response.data);
  } catch (error) {
    console.error('Error fetching packet stats config:', error.message);
    res.status(500).json({ error: 'Failed to fetch packet stats config' });
  }
});

// SSE endpoint for real-time data
app.get('/events', (req, res) => {
  res.writeHead(200, {
    'Content-Type': 'text/event-stream',
    'Connection': 'keep-alive',
    'Cache-Control': 'no-cache',
    'Access-Control-Allow-Origin': '*'
  });

  // Send initial data
  res.write(`data: ${JSON.stringify({ type: 'connected', message: 'SSE connection established' })}\n\n`);

  // Send periodic keep-alive message
  const interval = setInterval(() => {
    res.write(`data: ${JSON.stringify({ type: 'ping', timestamp: Date.now() })}\n\n`);
  }, 30000);

  // Close connection handler
  req.on('close', () => {
    clearInterval(interval);
  });
});

// Create WebSocket connections to the original service
let packetsWs, propagationsWs;

const setupWebSocketProxy = () => {
  // Close existing connections if they exist
  if (packetsWs) {
    packetsWs.close();
  }
  if (propagationsWs) {
    propagationsWs.close();
  }

  // WebSocket for packets
  packetsWs = new WebSocket('wss://www.meshcoretel.ru/ws/packets');

  packetsWs.on('open', () => {
    console.log('Connected to packets WebSocket');
    connectionStatus = 'Connected to packets WS';
    // Broadcast status update to all clients
    broadcastToSSE({ type: 'statusUpdate', connectionStatus: connectionStatus });
  });

  packetsWs.on('message', (data) => {
    try {
      // Forward message to clients connected to our server
      broadcastToSSE({ type: 'packet', data: data.toString() });
    } catch (error) {
      console.error('Error processing packet message:', error);
    }
  });

  packetsWs.on('close', () => {
    console.log('Packets WebSocket closed. Reconnecting...');
    connectionStatus = 'Packets WS disconnected';
    // Broadcast status update to all clients
    broadcastToSSE({ type: 'statusUpdate', connectionStatus: connectionStatus });
    setTimeout(setupWebSocketProxy, 5000);
  });

  packetsWs.on('error', (error) => {
    console.error('Packets WebSocket error:', error);
    connectionStatus = 'Packets WS error';
    // Broadcast status update to all clients
    broadcastToSSE({ type: 'statusUpdate', connectionStatus: connectionStatus });
  });

  // WebSocket for propagations
  propagationsWs = new WebSocket('wss://www.meshcoretel.ru/ws/propagations');

  propagationsWs.on('open', () => {
    console.log('Connected to propagations WebSocket');
    connectionStatus = 'Connected to propagations WS';
    // Broadcast status update to all clients
    broadcastToSSE({ type: 'statusUpdate', connectionStatus: connectionStatus });
  });

  propagationsWs.on('message', (data) => {
    try {
      // Forward message to clients connected to our server
      broadcastToSSE({ type: 'propagation', data: data.toString() });
    } catch (error) {
      console.error('Error processing propagation message:', error);
    }
  });

  propagationsWs.on('close', () => {
    console.log('Propagations WebSocket closed. Reconnecting...');
    connectionStatus = 'Propagations WS disconnected';
    // Broadcast status update to all clients
    broadcastToSSE({ type: 'statusUpdate', connectionStatus: connectionStatus });
    setTimeout(setupWebSocketProxy, 5000);
  });

  propagationsWs.on('error', (error) => {
    console.error('Propagations WebSocket error:', error);
    connectionStatus = 'Propagations WS error';
    // Broadcast status update to all clients
    broadcastToSSE({ type: 'statusUpdate', connectionStatus: connectionStatus });
  });
};

// Global connection status
let connectionStatus = 'Initializing...';

// SSE clients storage
const sseClients = [];

const broadcastToSSE = (data) => {
  sseClients.forEach(client => {
    try {
      client.res.write(`data: ${JSON.stringify(data)}\n\n`);
    } catch (error) {
      console.error('Error sending SSE:', error);
      // Remove client if there's an error
      const index = sseClients.indexOf(client);
      if (index > -1) {
        sseClients.splice(index, 1);
      }
    }
  });
};

// Handle SSE connections
app.get('/sse', (req, res) => {
  console.log('SSE client connected');
  res.writeHead(200, {
    'Content-Type': 'text/event-stream',
    'Connection': 'keep-alive',
    'Cache-Control': 'no-cache',
    'Access-Control-Allow-Origin': '*'
  });

  const clientId = Date.now();
  const client = {
    id: clientId,
    res
  };

  sseClients.push(client);

  // Send initial data
  res.write(`data: ${JSON.stringify({
    type: 'connected',
    message: 'SSE connection established',
    connectionStatus: connectionStatus
  })}\n\n`);

  // Send connection status updates
  const statusInterval = setInterval(() => {
    try {
      res.write(`data: ${JSON.stringify({
        type: 'statusUpdate',
        connectionStatus: connectionStatus
      })}\n\n`);
    } catch (error) {
      // If there's an error sending, client likely disconnected
      clearInterval(statusInterval);
    }
  }, 5000); // Update status every 5 seconds

  // Remove client when connection closes
  req.on('close', () => {
    console.log('SSE client disconnected');
    const index = sseClients.indexOf(client);
    if (index > -1) {
      sseClients.splice(index, 1);
    }
    clearInterval(statusInterval);
  });
});

// Start the server
const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
  console.log(`Server running on port ${PORT}`);
  console.log(`Web UI (optional): http://localhost:${PORT}`);
  
  // Set up WebSocket proxy connections
  setupWebSocketProxy();
});
