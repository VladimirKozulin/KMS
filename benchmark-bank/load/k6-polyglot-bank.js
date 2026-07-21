import http from 'k6/http';
import { check, sleep } from 'k6';

export const options = {
  stages: [
    { duration: '30s', target: 50 }, // Разогрев
    { duration: '5m', target: 200 }, // Постоянная эмуляция банковских транзакций
    { duration: '30s', target: 0 },
  ],
};

export default function () {
  // 1. Запрос к Node.js BFF Gateway (HTTP 200 OK)
  const resNode = http.get('http://node-bff-service:3000');
  check(resNode, { 'Node status 200': (r) => r.status === 200 });

  // 2. Запрос к Go Payment Service (HTTP 200 OK)
  const resGo = http.get('http://go-payment-service:5678');
  check(resGo, { 'Go status 200': (r) => r.status === 200 });

  sleep(0.1);
}
