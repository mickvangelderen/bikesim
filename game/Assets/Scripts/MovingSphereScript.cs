﻿using UnityEngine;
using System.Collections;

public class MovingSphereScript : MonoBehaviour {

	// Use this for initialization
	void Start () {
	
	}
	
	// Update is called once per frame
	void Update () {
	
	}

	void FixedUpdate () {

		float h = Input.GetAxis ("Horizontal");
		float v = Input.GetAxis ("Vertical");
		//float r = Input.GetAxis ("Rotation");

		if (h != 0) {
			this.rigidbody.AddTorque(0,h,0);	
		}
		
		if (v != 0) {
			this.rigidbody.AddRelativeTorque(v,0,0);	
		}
	}
}