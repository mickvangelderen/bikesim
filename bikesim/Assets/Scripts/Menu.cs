using UnityEngine;
using UnityEngine.UI;
using UnityEngine.SceneManagement;

public class Menu : MonoBehaviour {
    public Canvas mainCanvas;
    public InputField serialDeviceInputField;

    void Awake() {
        mainCanvas.enabled = true;
    }

    void Start() {
        serialDeviceInputField.onEndEdit.AddListener(value =>
            {
                GamePrefs.device = serialDeviceInputField.text;
                if (Input.GetKeyDown(KeyCode.Return) || Input.GetKeyDown(KeyCode.KeypadEnter)) {
                    SceneManager.LoadScene("Main");
                }
            });
    }
}
