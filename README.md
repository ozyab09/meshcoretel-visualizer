# MeshCoreTel Network Visualizer

A real-time visualization tool for the MeshCoreTel network, displaying nodes, connections, and data propagation across the network.

## Features

- Real-time visualization of MeshCoreTel network nodes
- Animated propagation paths showing data transmission
- Different colors for different node types:
  - Green: Chat Nodes
  - Blue: Repeaters
  - Yellow: Room Servers
  - Red: Sensors
- Live packet information display
- Interactive map centered on Moscow
- WebSocket integration for real-time updates

## Requirements

- Node.js (v14 or higher)
- npm or yarn

## Installation

1. Clone the repository:
```bash
git clone <repository-url>
cd meshcoretel-visualizer
```

2. Install dependencies:
```bash
npm install
```

## Running the Application

1. Start the server:
```bash
npm start
```

2. Open your browser and navigate to:
```
http://localhost:3000
```

## Project Structure

```
├── server.js           # Main server implementation
├── public/
│   ├── index.html      # Main HTML page
│   └── ...
├── package.json        # Project dependencies and scripts
└── README.md           # This file
```

## Configuration

The application connects to the MeshCoreTel API endpoints automatically. No additional configuration is required for basic operation.

## API Endpoints

- `/api/adverts` - Retrieves all network nodes with pagination
- `/api/observers` - Gets observer information
- `/api/packets` - Fetches packet data
- `/api/propagations` - Gets propagation data
- `/sse` - Server-Sent Events for real-time updates

## How It Works

The application connects to the MeshCoreTel network and visualizes:
1. Network nodes on a map centered on Moscow
2. Real-time data transmission between nodes
3. Propagation paths when messages travel through the network
4. Status information and statistics

## License

MIT