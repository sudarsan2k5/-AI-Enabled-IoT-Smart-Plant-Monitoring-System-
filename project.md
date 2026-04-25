# -AI-Enabled-IoT-Smart-Plant-Monitoring-System

# 🌱 AI-Enabled IoT Smart Plant Monitoring System

## Project Overview  
We are building an AI-enabled IoT-based smart plant monitoring system designed to monitor environmental and soil conditions and provide intelligent recommendations to improve plant health and productivity.

---

## System Components  

### Hardware:
- ESP32 microcontroller  
- DHT22 (temperature & humidity sensor)  
- Soil moisture sensor  
- LDR sensor (light intensity)  
- NPK sensor (Nitrogen, Phosphorus, Potassium)  
- RS485 to TTL converter  

---

## System Architecture  
Sensors → ESP32 → Backend API → Database → Dashboard → Recommendation System


---

## Data Flow  

1. Sensors collect real-time data  
2. ESP32 reads sensor values  
3. ESP32 sends data to backend via HTTP (JSON format)  
4. Backend processes and stores data in database  
5. Dashboard fetches and displays data  
6. Recommendation system analyzes data and generates insights  

---

## Data Format (JSON Example)

```json
{
  "temperature": 28,
  "humidity": 60,
  "soil_moisture": 45,
  "light": 300,
  "nitrogen": 20,
  "phosphorus": 15,
  "potassium": 10,
  "timestamp": "2026-04-22T10:00:00Z"
}
```

# Backend Requirements
- we will use python with fastapi
- we will also create some endpoints to send and receive the date
   for example:
       POST /sensor-data → receive data from ESP32
       GET /data → send data to dashboard
- Handle and validate incoming data
- Store data in database (sqlite db)