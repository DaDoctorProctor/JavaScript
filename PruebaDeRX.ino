void setup() {
  Serial.begin(115200);
}

void loop() {
  if (Serial.available() > 0) {
    char incomingByte;
    incomingByte = Serial.read();
    Serial.print(incomingByte);
  }
}
