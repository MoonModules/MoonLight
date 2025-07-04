/**
    @title     MoonLight
    @file      PhysicalLayer.cpp
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/general/utilities/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact moonmodules@icloud.com
**/

#if FT_MOONLIGHT

#include "PhysicalLayer.h"

#include "VirtualLayer.h"

#include "Nodes.h"

#include <ESP32SvelteKit.h> //for safeModeMB

PhysicalLayer::PhysicalLayer() {
        ESP_LOGD(TAG, "constructor");

        memset(lights.channels, 0, MAX_CHANNELS); // set all the channels to 0

        // initLightsToBlend();

        //create one layer - temporary
        layerV.push_back(new VirtualLayer());
        layerV[0]->layerP = this;
    }

    void PhysicalLayer::setup() {
        for (VirtualLayer * layer: layerV) {
            layer->setup();
        }
    }

    //run one loop of an effect
    void PhysicalLayer::loop() {
        //runs the loop of all effects / nodes in the layer
        for (VirtualLayer * layer: layerV) {
            if (layer) layer->loop(); //if (layer) needed when deleting rows ...
        }
    }
    
    void PhysicalLayer::addLayoutPre() {
        lights.header.nrOfLights = 0; // for pass1 and pass2 as in pass2 virtual layer needs it
        ESP_LOGD(TAG, "pass %d %d", pass, lights.header.nrOfLights);

        if (pass == 1) {
            lights.header.size = {0,0,0};
            lights.header.isPositions = 1; //in progress...
            delay(100); //wait to stop effects
            //set all channels to 0 (e.g for multichannel to not activate unused channels, e.g. fancy modes on MHs)
            memset(lights.channels, 0, MAX_CHANNELS); // set all the channels to 0
            //dealloc pins
            sortedPins.clear(); //clear the added pins for the next pass
        } else if (pass == 2) {
            for (VirtualLayer * layer: layerV) {
                //add the lights in the virtual layer
                layer->addLayoutPre();
            }
        }
    }

    void PhysicalLayer::addLight(Coord3D position) {

        if (safeModeMB && lights.header.nrOfLights > 1023) {
            // ESP_LOGW(TAG, "Safe mode enabled, not adding lights > 1023");
            return;
        }

        if (pass == 1) {
            // ESP_LOGD(TAG, "%d,%d,%d", position.x, position.y, position.z);
            if (lights.header.nrOfLights >= MAX_CHANNELS / sizeof(Coord3D)) {
                //send the positions to the UI _socket_emit
                //reset the index and continue...
            } else
                lights.positions[lights.header.nrOfLights] = {position.x, position.y, position.z};
            
            lights.header.size = lights.header.size.maximum(position);
        } else {
            for (VirtualLayer * layer: layerV) {
                //add the position in the virtual layer
                layer->addLight(position);
            }
        }
        lights.header.nrOfLights++;
    }

    void PhysicalLayer::addPin(uint8_t pinNr) {
        if (pass == 1) {
            ESP_LOGD(TAG, "addPin %d %d", pinNr, lights.header.nrOfLights);

            SortedPin previousPin;
            if (!sortedPins.empty()) {
                previousPin = sortedPins.back(); //get the last added pin
                if (previousPin.pin == pinNr) { //if the same pin, just increase the number of leds
                    previousPin.nrOfLights += lights.header.nrOfLights - previousPin.startLed;
                    sortedPins.back() = previousPin; //update the last added pin
                    return; //no need to add a new pin
                }
            } else {
                previousPin.startLed = 0; //first pin, start at 0
                previousPin.nrOfLights = 0;
            }

            //0 400
            //400 400
            //800 400

            SortedPin sortedPin;
            sortedPin.startLed = previousPin.startLed + previousPin.nrOfLights; //start at the end of the previous pin
            sortedPin.nrOfLights = lights.header.nrOfLights - sortedPin.startLed;
            sortedPin.pin = pinNr; //the pin number

            sortedPins.push_back(sortedPin);
        }
    }

    void PhysicalLayer::addLayoutPost() {
        if (pass == 1) {
            lights.header.size += Coord3D{1,1,1};
            ESP_LOGD(TAG, "pass %d #:%d s:%d,%d,%d (%d=%d+%d)", pass, lights.header.nrOfLights, lights.header.size.x, lights.header.size.y, lights.header.size.z, sizeof(Lights), sizeof(LightsHeader), sizeof(lights.leds));
            //send the positions to the UI _socket_emit
            lights.header.isPositions = 10; //filled with positions, set back to ct_Leds in ModuleEditor

            // initLightsToBlend();

            // if pins defined
            if (!sortedPins.empty()) {

                //sort the vector by the starLed
                std::sort(sortedPins.begin(), sortedPins.end(), [](const SortedPin &a, const SortedPin &b) {return a.startLed < b.startLed;});

                ledsDriver.init(lights, sortedPins); //init the driver with the sorted pins and lights

            } //pins defined
        } else if (pass == 2) {
            ESP_LOGD(TAG, "pass %d %d", pass, lights.header.nrOfLights);
            for (VirtualLayer * layer: layerV) {
                //add the position in the virtual layer
                layer->addLayoutPost();
            }
        }
    }

    // an effect is using a virtual layer: tell the effect in which layer to run...

    //run one loop of an effect
    Node* PhysicalLayer::addNode(const uint8_t index, const char * name, const JsonArray controls) {

        Node *node = nullptr;
        if (equal(name, SolidEffect::name())) node = new SolidEffect();
        //alphabetically from here
        else if (equal(name, BouncingBallsEffect::name())) node = new BouncingBallsEffect();
        else if (equal(name, DistortionWavesEffect::name())) node = new DistortionWavesEffect();
        else if (equal(name, FreqMatrixEffect::name())) node = new FreqMatrixEffect();
        else if (equal(name, GEQEffect::name())) node = new GEQEffect();
        else if (equal(name, LinesEffect::name())) node = new LinesEffect();
        else if (equal(name, LissajousEffect::name())) node = new LissajousEffect();
        else if (equal(name, PaintBrushEffect::name())) node = new PaintBrushEffect();
        else if (equal(name, RainbowEffect::name())) node = new RainbowEffect();
        else if (equal(name, RandomEffect::name())) node = new RandomEffect();
        else if (equal(name, RipplesEffect::name())) node = new RipplesEffect();
        else if (equal(name, RGBWParEffect::name())) node = new RGBWParEffect();
        else if (equal(name, SinusEffect::name())) node = new SinusEffect();
        else if (equal(name, SphereMoveEffect::name())) node = new SphereMoveEffect();
        else if (equal(name, FixedRectangleEffect::name())) node = new FixedRectangleEffect();
        else if (equal(name, WaveEffect::name())) node = new WaveEffect();
        else if (equal(name, MHTroy15Effect::name())) node = new MHTroy15Effect();
        else if (equal(name, MHTroy32Effect::name())) node = new MHTroy32Effect();
        else if (equal(name, MHWowiEffect::name())) node = new MHWowiEffect();

        else if (equal(name, HumanSizedCubeLayout::name())) node = new HumanSizedCubeLayout();
        else if (equal(name, PanelLayout::name())) node = new PanelLayout();
        else if (equal(name, RingsLayout::name())) node = new RingsLayout();
        else if (equal(name, SingleLineLayout::name())) node = new SingleLineLayout();
        else if (equal(name, SingleRowLayout::name())) node = new SingleRowLayout();

        //custom
        else if (equal(name, SE16Layout::name())) node = new SE16Layout();
        else if (equal(name, MHTroy15Layout::name())) node = new MHTroy15Layout();
        else if (equal(name, MHTroy32Layout::name())) node = new MHTroy32Layout();
        else if (equal(name, MHWowi24Layout::name())) node = new MHWowi24Layout();

        else if (equal(name, CircleModifier::name())) node = new CircleModifier();
        else if (equal(name, MirrorModifier::name())) node = new MirrorModifier();
        else if (equal(name, MultiplyModifier::name())) node = new MultiplyModifier();
        else if (equal(name, PinwheelModifier::name())) node = new PinwheelModifier();
        else if (equal(name, RotateNodifier::name())) node = new RotateNodifier();
        
        else if (equal(name, AudioSyncMod::name())) node = new AudioSyncMod();
        #if FT_LIVESCRIPT
            else {
                LiveScriptNode *liveScriptNode = new LiveScriptNode();
                liveScriptNode->animation = name; //set the (file)name of the script
                node = liveScriptNode;
            }
        #endif

        if (node) {
            node->constructor(layerV[0], controls); //pass the layer to the node
            node->setup(); //run the setup of the effect
            // layerV[0]->nodes.reserve(index+1);
            if (index >= layerV[0]->nodes.size())
                layerV[0]->nodes.push_back(node);
            else
                layerV[0]->nodes[index] = node; //add the node to the layer
        }

        ESP_LOGD(TAG, "%s (s:%d p:%p)", name, layerV[0]->nodes.size(), node);

        return node;
    }

    void PhysicalLayer::removeNode(Node *node) {
        ESP_LOGD(TAG, "remove node (s:%d p:%p)", layerV[0]->nodes.size(), node);
        // node->destructor(); //now ˜destructor is called in Node destructor
        // delete node; //causing assert failed: multi_heap_free multi_heap_poisoning.c:259 (head != NULL) ATM
        // //can cause a crash if it has addControl ...  (e.g. panel layout)
        
        // layerV[0]->nodes[index] = nullptr;
    }

    // // to be called in setup, if more then one effect
    // void PhysicalLayer::initLightsToBlend() {
    //     lightsToBlend.reserve(lights.header.nrOfLights);

    //     for (uint16_t indexP = 0; indexP < lightsToBlend.size(); indexP++)
    //       lightsToBlend[indexP] = false;
    // }

#endif //FT_MOONLIGHT