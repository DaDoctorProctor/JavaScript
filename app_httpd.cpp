#include "dl_lib_matrix3d.h"
#include <esp32-hal-ledc.h>
int speed = 255;
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "Arduino.h"
//Extra
#include "camera_index.h"
//extern int val_final;
extern int val_final;



typedef struct {
  httpd_req_t *req;
  size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

static size_t jpg_encode_stream(void * arg, size_t index, const void* data, size_t len) {
  jpg_chunking_t *j = (jpg_chunking_t *)arg;
  if (!index) {
    j->len = 0;
  }
  if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK) {
    return 0;
  }
  j->len += len;
  return len;
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  int64_t fr_start = esp_timer_get_time();

  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");

  size_t out_len, out_width, out_height;
  uint8_t * out_buf;
  bool s;
  {
    size_t fb_len = 0;
    if (fb->format == PIXFORMAT_JPEG) {
      fb_len = fb->len;
      res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    } else {
      jpg_chunking_t jchunk = {req, 0};
      res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk) ? ESP_OK : ESP_FAIL;
      httpd_resp_send_chunk(req, NULL, 0);
      fb_len = jchunk.len;
    }
    esp_camera_fb_return(fb);
    int64_t fr_end = esp_timer_get_time();
    Serial.printf("JPG: %uB %ums\n", (uint32_t)(fb_len), (uint32_t)((fr_end - fr_start) / 1000));
    return res;
  }

  dl_matrix3du_t *image_matrix = dl_matrix3du_alloc(1, fb->width, fb->height, 3);
  if (!image_matrix) {
    esp_camera_fb_return(fb);
    Serial.println("dl_matrix3du_alloc failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  out_buf = image_matrix->item;
  out_len = fb->width * fb->height * 3;
  out_width = fb->width;
  out_height = fb->height;

  s = fmt2rgb888(fb->buf, fb->len, fb->format, out_buf);
  esp_camera_fb_return(fb);
  if (!s) {
    dl_matrix3du_free(image_matrix);
    Serial.println("to rgb888 failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  jpg_chunking_t jchunk = {req, 0};
  s = fmt2jpg_cb(out_buf, out_len, out_width, out_height, PIXFORMAT_RGB888, 90, jpg_encode_stream, &jchunk);
  dl_matrix3du_free(image_matrix);
  if (!s) {
    Serial.println("JPEG compression failed");
    return ESP_FAIL;
  }

  int64_t fr_end = esp_timer_get_time();
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];
  dl_matrix3du_t *image_matrix = NULL;

  static int64_t last_frame = 0;
  if (!last_frame) {
    last_frame = esp_timer_get_time();
  }

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      {
        if (fb->format != PIXFORMAT_JPEG) {
          bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
          esp_camera_fb_return(fb);
          fb = NULL;
          if (!jpeg_converted) {
            Serial.println("JPEG compression failed");
            res = ESP_FAIL;
          }
        } else {
          _jpg_buf_len = fb->len;
          _jpg_buf = fb->buf;
        }
      }
    }
    if (res == ESP_OK) {
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if (res != ESP_OK) {
      break;
    }
    int64_t fr_end = esp_timer_get_time();
    int64_t frame_time = fr_end - last_frame;
    last_frame = fr_end;
    frame_time /= 1000;
    /*Serial.printf("MJPG: %uB %ums (%.1ffps)\n",
                  (uint32_t)(_jpg_buf_len),
                  (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time
                 );*/
  }

  last_frame = 0;
  return res;
}


static esp_err_t cmd_handler(httpd_req_t *req)
{
  char*  buf;
  size_t buf_len;
  char variable[32] = {0,};
  char value[32] = {0,};

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char*)malloc(buf_len);
    if (!buf) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) == ESP_OK &&
          httpd_query_key_value(buf, "val", value, sizeof(value)) == ESP_OK) {
      } else {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
      }
    } else {
      free(buf);
      httpd_resp_send_404(req);
      return ESP_FAIL;
    }
    free(buf);
  } else {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  int val = atoi(value);
  sensor_t * s = esp_camera_sensor_get();
  int res = 0;

  if (!strcmp(variable, "framesize"))
  {
    Serial.println("framesize");
    if (s->pixformat == PIXFORMAT_JPEG) res = s->set_framesize(s, (framesize_t)val);
  }
  else if (!strcmp(variable, "quality"))
  {
    Serial.println("quality");
    res = s->set_quality(s, val);
  }


//--------------------------------------------------------------
  //Remote Control Car
  //Don't use channel 1 and channel 2
  else if (!strcmp(variable, "flash"))
  {ledcWrite(7, val);}
  else if (!strcmp(variable, "sm1")){
    //ledcWrite(3,val);
    val_final = 1000 + val;
  }
  else if (!strcmp(variable, "sm2")){
    //ledcWrite(4, val);
    val_final = 2000 + val;
  }
  else if (!strcmp(variable, "sm3")){
    //ledcWrite(5, val);
    //int val_03 = 3000 + val;
    val_final = 3000 + val;
  }
  else if (!strcmp(variable, "sm4")){
    //ledcWrite(6, val);
    //int val_04 = 4000 + val;
    val_final = 4000 + val;
  } 
  else if (!strcmp(variable, "car")) {
    if (val == 1) {
      //Serial.println("Forward");
      ledcWrite(8, 60);
      Serial.println("arriba");
    }
    else if (val == 2) {
      //Serial.println("Turn Left");
      ledcWrite(8, 120);
      Serial.println("izquierdo");
    }
    else if (val == 3) {
      //Serial.println("Stop");
      ledcWrite(8, 0);
      Serial.println("stop");
    }
    else if (val == 4) {
      //Serial.println("Turn Right");
      ledcWrite(8, 200);
      Serial.println("derecho");
    }
    else if (val == 5) {
      //Serial.println("Backward");
      ledcWrite(8, 255);
      Serial.println("abajo");
    }
  }
  else
  {
    Serial.println("variable");
    res = -1;
  }

  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}


static esp_err_t status_handler(httpd_req_t *req) {
  static char json_response[1024];

  sensor_t * s = esp_camera_sensor_get();
  char * p = json_response;
  *p++ = '{';

  p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
  p += sprintf(p, "\"quality\":%u,", s->status.quality);
  *p++ = '}';
  *p++ = 0;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json_response, strlen(json_response));
}

/* Index page */
static const char PROGMEM INDEX_HTML[] = R"rawliteral(

<!doctype html>
<html>
    <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
        <title>Robot modular</title>
        <style>
            .button {background-color: #000000;
        border: none;
        border-radius: 4px;
        color: white;
        padding: 2vh 3vh;
        text-align: center;
        font-size: 3.5vh;
        margin: 4px 2px;
        cursor: pointer;
        width: 20%;
        border: 2px solid black;
        }
      
      .slider {
        appearance: none;
        width: 70%;height: 2vh;
        border-radius: 1.5vh;
        background: #d3d3d3;
        outline: none;
        -webkit-transition: .2s;
        transition: opacity .2s;
        }
      
      .slider::-webkit-slider-thumb {appearance: none;
        appearance: none;
        width: 3vh;
        height: 3vh;
        border-radius: 50%;
        background: #000000;
        }
      
      .slider::-moz-range-thumb {
        width: 25px;
        height: 25px;
        background: #4CAF50;
        cursor: pointer;
        }
      
      .label {color: #000000;
        font-size: 3vh;
        }
        
      .writing_txt {color: #000000;
        font-size: 2vh;
        }
      
      input[type="range"] {
        -webkit-appearance: none;
        width: 60%px;
        background-color: #666;
        cursor: ew-resize;
        overflow: hidden;
        }

      input[type="range"]::-webkit-slider-thumb {
        -webkit-appearance: none;
        background: #fff;
        box-shadow: -50vh 0 0 50vh hsl(100,100%,40%);
        }
      
      body { font-family: Arial, sans-serif;
        margin: 0; padding: 0;
        background: black;
        overflow: hidden;
        };
      
      .button1 {
        background-color: black; 
        color: white;
        }

      .button1:active {
        background-color: white;
        color: black;
        border: 2px solid black;
        }
      
      /* Style the tab */
      .tab {
        overflow: hidden;
        background-color: black;
        border: 0px
        }

      /* Style the buttons inside the tab */
      .tab button {
        background-color: black;
        color: white;
        float: left;
        border: none;
        outline: none;
        cursor: pointer;
        padding: 0px 3.3vh;
        transition: 0.3s;
        font-size: 1.5vh;
        height: 3.6vh;
        }

      /* Change background color of buttons on hover */
      .tab button:hover {
        background-color: #ddd;
        }

      /* Create an active/current tablink class */
      .tab button.active {
        background-color: #ccc;
        }

      /* Style the tab content */
      .tabcontent {
        display: none;
        background-color: white;
        height: 40vh;
        }
      
      /* RGB BAR */
      .wrapper{
        height: 1.5vh;
        width: 100%;
        position: relative;
        background: linear-gradient(135deg, #14ffe9, #ffeb3b, #ff00e0);
        border-radius: 0px;
        cursor: default;
        animation: animate 1.5s linear infinite;
        }

      .wrapper .display,
      .wrapper span{
        position: absolute;
        top: 50%;
        left: 50%;
        transform: translate(-50%, -50%);
        }
        
      .wrapper .display{
        z-index: 999;
        height: 30px;
        width: 20px;
        background: #1b1b1b;
        border-radius: 6px;
        text-align: center;
        }
  
      .display #doc{
        line-height: 2vh;
        color: #fff;
        font-size: 2vh;
        font-weight: 1;
        letter-spacing: 1px;
        background: linear-gradient(135deg, #14ffe9, #ffeb3b, #ff00e0);
        -webkit-background-clip: text;
        -webkit-text-fill-color: transparent;
        animation: animate 1.5s linear infinite;
        }
      .display #julian{
        line-height: 2vh;
        color: #fff;
        font-size: 2vh;
        font-weight: 1;
        letter-spacing: 1px;
        background: linear-gradient(135deg, #14ffe9, #ffeb3b, #ff00e0);
        -webkit-background-clip: text;
        -webkit-text-fill-color: transparent;
        animation: animate 1.5s linear infinite;
        }
      .display #grunt{
        line-height: 2vh;
        color: #fff;
        font-size: 2vh;
        font-weight: 1;
        letter-spacing: 1px;
        background: linear-gradient(135deg, #14ffe9, #ffeb3b, #ff00e0);
        -webkit-background-clip: text;
        -webkit-text-fill-color: transparent;
        animation: animate 1.5s linear infinite;
        }
      .display #ventura{
        line-height: 2vh;
        color: #fff;
        font-size: 2vh;
        font-weight: 1;
        letter-spacing: 1px;
        background: linear-gradient(135deg, #14ffe9, #ffeb3b, #ff00e0);
        -webkit-background-clip: text;
        -webkit-text-fill-color: transparent;
        animation: animate 1.5s linear infinite;
        }
  
      @keyframes animate {
        100%{
        filter: hue-rotate(360deg);}
        }
        
      .wrapper span{
        height: 10%;
        width: 100%;
        border-radius: 10px;
        background: inherit;
        }
      .wrapper span:first-child{
        filter: blur(7px);
        }
      .wrapper span:last-child{
        filter: blur(20px);
        }
  
      img {
        display: block;
        }
  
      .sliderAlign {
        padding-left: 6vh;
        padding-right: 6vh;
        padding-top: 1%;
        }
        
      .SpecsAlign {
        float: left;
        width: 75vh;
        padding-left: 10vh;
        padding-top: 1%;
        }
      
      .DataAlign {
        float: left;
        width: 75vh;
        padding-left: 15vh;
        padding-top: 3%;
        }
    
      /* Style the close button */
      .closetab {
        float: right;
        cursor: pointer;
        font-size: 20px;
        height: 20px;
        color: black
        }

      .closetab:hover {color: red;}
  
    </style>
    </head>
    <body>
  
    <div align=center> 
      <img src='http://192.168.4.1:81/stream' style='width:100%; transform:rotate(180deg);'>
    </div>
    
    <div class="wrapper"></div> 
    
    <div class="tab">
      <button class="tablinks" onclick="tabAdder(event, 'Control');myDIV.style.display='none'" id="defaultOpen">Controles</button>
      <button class="tablinks" onclick="tabAdder(event, 'Servomotors');myDIV.style.display='none'">Servomotores</button>
      <button class="tablinks" onclick="tabAdder(event, 'About');myDIV.style.display='none'">Acerca de</button>
      <span onclick="myDIV.style.display='block'"; class="closetab" id="defaultOpen">&times</span>
      <button class="tablinks" onclick="tabAdder(event, 'Debug')" id="myDIV">Debug</button>
      
    </div>

    <div id="Control" class="tabcontent">
      
      <br/>
      <div align=center> 
        <button class="button button1" id="forward" ontouchstart="fetch(document.location.origin+'/control?var=car&val=1');" 
        ontouchend="fetch(document.location.origin+'/control?var=car&val=3');" >△</button>
      </div>
    
      <div align=center>  
        <button class="button button1" id="turnleft" ontouchstart="fetch(document.location.origin+'/control?var=car&val=2');" 
        ontouchend="fetch(document.location.origin+'/control?var=car&val=3');" >◁</button>
        <button class="button button1" id="turnright" ontouchstart="fetch(document.location.origin+'/control?var=car&val=4');" 
        ontouchend="fetch(document.location.origin+'/control?var=car&val=3');" >▷</button>     
      </div>
    
      
      <div align=center> 
        <button class="button button1" id="backward" ontouchstart="fetch(document.location.origin+'/control?var=car&val=5');" 
        ontouchend="fetch(document.location.origin+'/control?var=car&val=3');">▽</button>
      </div>
    </div>

    <div id="Servomotors" class="tabcontent">
      <br/>
    
      <div class="sliderAlign">  
        <label class="label">Flash</label>
        <input type="range" class="slider" id="flash" min="0" max="255" value="0" 
        oninput="try{fetch(document.location.origin+'/control?var=flash&val='+this.value);}catch(e){}"
        oninput="Conversion(flash.value,0)">
        <label class="label" id="convFlash">0</label>
      </div>
      <br/>

      <div class="sliderAlign"> 
        <label class="label">SmA</label>
        <input type="range" class="slider" id="sm1" min="0" max="255" value="0" 
        oninput="try{fetch(document.location.origin+'/control?var=sm1&val='+this.value);}catch(e){};
        Conversion(sm1.value,1)">
        <label class="label" id="convSM1">0</label>
      </div>
      <br/>
    
      <div class="sliderAlign"> 
        <label class="label">SmB</label>
        <input type="range" class="slider" id="sm2" min="0" max="255" value="0" 
        oninput="try{fetch(document.location.origin+'/control?var=sm2&val='+this.value);}catch(e){}; 
        Conversion(sm2.value,2)">
        <label class="label" id="convSM2">0</label>
      </div>
      <br/>
    
      <div class="sliderAlign"> 
        <label class="label">SmC </label>
        <input type="range" class="slider" id="sm3" min="0" max="255" value="230" 
        oninput="try{fetch(document.location.origin+'/control?var=sm3&val='+this.value);}catch(e){};
        Conversion(sm3.value,3)">
        <label class="label" id="convSM3">90</label>
      </div>
      <br/>
    
      <div class="sliderAlign"> 
        <label class="label">SmD</label>
        <input type="range" class="slider" id="sm4" min="0" max="255" value="127.5" 
        oninput="try{fetch(document.location.origin+'/control?var=sm4&val='+this.value);}catch(e){}; 
        Conversion(sm4.value,4)">
        <label class="label" id="convSM4">50</label>
      </div>
      
      <br/>
    
    </div>
  
    <div id="About" class="tabcontent">
      <div class="SpecsAlign"> 
        <BR>
        
        <label class="label writing_txt">Especificaciones:</label>
        <br>
        <label class="label writing_txt">Principal:</label>
        <br>
        <label class="label writing_txt">CPU: </label> 
        <br>
        <label class="label writing_txt">ESP32, Dual Core 32-bit LX6 240Mhz.</label>
        <br>
        <label class="label writing_txt">RAM: 520KB SRAM.</label>
        <br>
        <label class="label writing_txt">Up to 160MHz clock.</label>
        <br>
        <label class="label writing_txt">Partes: </label>
        <br>
        <label class="label writing_txt">Arduino Nano x5</label>
        
      
        <div class="display">
        <div id="doc"></div>
        </div>
        <div class="display">
          <div id="julian"></div>
        </div>
        <div class="display">
          <div id="grunt"></div>
        </div>
        <div class="display">
          <div id="ventura"></div>
        </div>
        <br>
        <label class="label">Departamento de Mecatronica</label>
      
      
      </div>  
      
    </div>
  
  
    <div id="Debug" class="tabcontent">
      
      <div align=center> 
        <button class="button button1" id="foward" 
        onclick="fetch(document.location.origin+'/control?var=car&val=1');">
      △</button>
      </div>
      
      <div align=center>  
        <button class="button button1" id="turnleft" 
        onclick="fetch(document.location.origin+'/control?var=car&val=2');">
        ◁</button>
        <button class="button button1" id="stop" 
        onclick="fetch(document.location.origin+'/control?var=car&val=3');">
        X</button>
        <button class="button button1" id="turnright" 
        onclick="fetch(document.location.origin+'/control?var=car&val=4');">
        ▷</button>
        
      </div>
      
      <div align=center> 
        <button class="button button1" id="backward"
        onclick="fetch(document.location.origin+'/control?var=car&val=5');">
        ▽</button>
      </div>

    </div>
    
    <script>
      var xtz = 0;
      var last_tabName = "";
      function tabAdder(evt, tabName) {
        var i, tabcontentZERO, tablinksZERO;
        
        tabcontentZERO = document.getElementsByClassName("tabcontent");
        
        var tabcontentArray = Array.from(tabcontentZERO);
        
        var tabcontent = tabcontentArray;
        
        //Get the tabs orderned in different parts
        for (i = 0; i < tabcontent.length; i++) {
          tabcontent[i].style.display = "none";
        }
        
        tablinksZERO = document.getElementsByClassName("tablinks");
        
        var tablinksArray = Array.from(tablinksZERO);
        var tablinks = tablinksArray;
        
        //Check for active tab color, name bar, do not modify.
        for (i = 0; i < tablinks.length; i++) {
          tablinks[i].className = tablinks[i].className.replace(" active", "");
        }
        
        //document.getElementById(tabName).style.display = "block";
        
        
        document.getElementById(tabName).style.display = "block";
        //Controls the Background latch.
        evt.currentTarget.className += " active";
        
      }
      
      document.getElementById("defaultOpen").click();
          
      function myFunction() {
        var x = document.getElementById("myDIV");
        if (x.style.display === "block") {
          x.style.display = "none";
        } else {
          x.style.display = "block";
        }
      }
      
      function Conversion(val,name){
        var x = val;
        var z = parseInt(x * 100 / 255) ;
        switch (name) {
          case 0:
            document.getElementById("convFlash").innerHTML = z;
            break;
          case 1:
            document.getElementById("convSM1").innerHTML = z;
            break;
          case 2:
            document.getElementById("convSM2").innerHTML = z;
            break;
          case 3:
            document.getElementById("convSM3").innerHTML = z;
            break;
          case 4:
            document.getElementById("convSM4").innerHTML = z; 
            break;            
          }
      }     

      setInterval(()=>{
        const doc = document.querySelector(".display #doc");
        const julian = document.querySelector(".display #julian");
        doc.textContent = "App designed by: Homero";
        julian.textContent = "Manufacturing by: Ramon";
        grunt.textContent = "Designed by: Ivan";
        ventura.textContent = "Documentation by: Abigail";
      });
    </script>
    
    </body>
</html>

)rawliteral";

static esp_err_t index_handler(httpd_req_t *req){
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)INDEX_HTML, strlen(INDEX_HTML));
}

//Main Loop
void startCameraServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t status_uri = {
        .uri       = "/status",
        .method    = HTTP_GET,
        .handler   = status_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t cmd_uri = {
        .uri       = "/control",
        .method    = HTTP_GET,
        .handler   = cmd_handler,
        .user_ctx  = NULL
    };

   httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };
    
    Serial.printf("Starting web server on port: '%d'\n", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &cmd_uri);
        httpd_register_uri_handler(camera_httpd, &status_uri);
    }

    config.server_port += 1;
    config.ctrl_port += 1;
    Serial.printf("Starting stream server on port: '%d'\n", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
    }
}
