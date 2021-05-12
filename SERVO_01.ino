// RECEPTOR 
// ESCLAVO_SERVO-01
// RJGR

const int casa = 45;
int val_current = 45;
int val_desired;

#include <Servo.h>
Servo myservo;

#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>
 
//Declaremos los pines CE y el CSN
#define CE_PIN 10 //CAMBIAR SEGUN EL ARDUINO 
#define CSN_PIN 9 //CAMBIAR SEGUN EL ARDUINO 
 
//Variable con la dirección del canal que se va a leer
byte direccion[5] ={'c','a','n','a','l'}; 

//creamos el objeto radio (NRF24L01)
RF24 radio(CE_PIN, CSN_PIN);

//vector para los datos recibidos
float datos[9];

void setup(){ 
	myservo.attach(3);
	myservo.write(casa); // 45 grados. 
	//inicializamos el NRF24L01 
	radio.begin();
	inicializamos el puerto serie
	Serial.begin(115200);
  delay(1000); 
	//Abrimos el canal de Lectura
	radio.openReadingPipe(1, direccion);
	//empezamos a escuchar por el canal
	radio.startListening();
}
 
void loop() {
	uint8_t numero_canal;
	if (radio.available()){    
		//Leemos los datos y los guardamos en la variable datos[]
		  radio.read(datos,sizeof(datos));

		//reportamos por el puerto serial los datos recibidos
	//----------------------------------------------------------  
	
  		int d_ang = datos[1] ;
  		int angulo = map(d_ang,0,255,0,180);
      int velocidad = 25; //Medido en Milisegundos
      //Correr setPosition_PalmaSalazar para realizar las evaluaciones de posicion.
      setPosition_PalmaSalazar(angulo,velocidad);
      delay(5000);
	}	
}

void setPosition_PalmaSalazar(int val_desired, int val_speed){
  //Mod H.P. - 12/04/2021
    //If the value is not between 180 and 0.
    if(val_desired > 180 || val_desired < 0){
        Serial.println("WARNING: ServoMotor value has to be between 0 and 180");
        delay(5000);
        return;
    }
    if(val_desired > val_current){
      //Si el valor deseado es mayor al valor actual entonces se aumenta el angulo del for.
      for (int i = val_current; i <= val_desired; i++){
        //Escritura de posicion al servomotor.
        myservo.write(i);
        val_current = i;
        Serial.print("Debug: Position ");
        Serial.println(i);
        delay(val_speed);
        //Si en cualquier momento se desea saber la posicion del servo consultar
        //val_current.
      }
      //Terminando hay que indicar el angulo final al valor actual
      
    }else if(val_desired < val_current){
      //De lo contrario si el valor deseado es menor al actual se disminiye el angulo for.
      for (int i = val_current; i >= val_desired; i--){
        myservo.write(i);
        Serial.print("Debug: Position ");
        Serial.println(i);
        delay(val_speed);
        val_current = i;
      }
      
    }else if(val_desired == val_current){
      //If the current value is the same as desired hold the position.
      val_desired = val_current;
      Serial.println("Debug: Holding current position");
      delay(1000);
    }
    Serial.println("Debug: Test 1 passed!");
}
