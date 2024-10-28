var option_count = 3;
var max_option_count = 32;

document.addEventListener('DOMContentLoaded', function(){
  var button = document.getElementById('add-poll-option');
  button.addEventListener('click', add_candidate);
});

function add_candidate() {
  if (option_count >= max_option_count) {
    return;
  }

  var parent = document.getElementById('poll-form-fieldset');
  var candidate_input = document.createElement('div');

  candidate_input.innerHTML = '<label for="poll">Option ' +
    (option_count + 1) +
    ': </label>' +
    '<input type="text" name="option"/>' +
    '</div>';
  parent.insertBefore(candidate_input, parent.childNodes[2 + option_count]);

  option_count += 1;
}
