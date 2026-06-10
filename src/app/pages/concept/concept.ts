import { Component } from '@angular/core';

interface ArchLevel {
  level: string;
  name: string;
  icon: string;
  chip: string;
  description: string;
  points: string[];
}

@Component({
  selector: 'app-concept',
  imports: [],
  templateUrl: './concept.html',
  styleUrl: './concept.scss',
})
export class Concept {
  protected readonly levels: ArchLevel[] = [
    {
      level: 'Level 1',
      name: 'Edge — Edge Computing Layer',
      icon: '📡',
      chip: 'ESP32-C3',
      description:
        'Drone nodes collect raw sensor data and pre-process it at the edge in real time, reducing upstream transmission load.',
      points: [
        'BMP180 — barometric pressure (altitude estimation)',
        'MPU6500 — accelerometer & gyroscope (attitude sensing)',
        'QMC5883L — magnetometer (heading)',
        'ESP32-C3 as the compute brain of each node',
      ],
    },
    {
      level: 'Level 2',
      name: 'Fog — Fog Computing Layer',
      icon: '🌫️',
      chip: 'ESP32-S3',
      description:
        'Receives data from Level 1 and computes the Zone Position Map locally, enabling low-latency swarm coordination.',
      points: [
        'The more powerful ESP32-S3 serves as the central node',
        'Computes the Zone Position Map',
        'Low-latency inter-node communication via the ESP-NOW protocol',
      ],
    },
    {
      level: 'Level 3',
      name: 'Cloud / Fog Interface Layer',
      icon: '☁️',
      chip: 'MQTT Broker',
      description:
        'Uploads the aggregated fog-layer information to the backend for global map updates, storage, and show-wide monitoring.',
      points: [
        'Aggregated data sent to the backend via the MQTT protocol',
        'Real-time update and storage of the Global AP Map',
        'Show scheduling and system status monitoring interface',
      ],
    },
  ];
}
